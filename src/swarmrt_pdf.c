/*
 * swarmrt_pdf.c — Pure C PDF text extraction engine
 *
 * Components (bottom-up):
 *   1. Lexer/tokenizer
 *   2. Cross-reference table parser (traditional + xref streams)
 *   3. Stream decompressors (Flate, ASCII85, ASCIIHex, LZW, RunLength, predictors)
 *   4. Encryption handler (RC4, AES-128-CBC)
 *   5. Font encoding / Unicode mapper (ToUnicode CMap, WinAnsi, MacRoman, Standard, AGL)
 *   6. Content stream interpreter (text state machine)
 *   7. Page tree walker + top-level API
 *
 * otonomy.ai
 */

#include "swarmrt_pdf.h"
#include "swarmrt_pdf_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <zlib.h>

/* === Configuration === */
#define PDF_MAX_OBJECTS      200000
#define PDF_MAX_DEPTH        64
#define PDF_MAX_STREAM       (256 * 1024 * 1024)  /* 256 MB max stream */
#define PDF_MAX_PAGES        50000
#define PDF_INITIAL_BUF      (64 * 1024)           /* 64 KB initial output */
#define PDF_MAX_FILTERS      8
#define PDF_MAX_GSTATE_STACK 64
#define PDF_CMAP_MAX         65536

/* === Forward declarations === */
typedef struct pdf_doc pdf_doc_t;
typedef struct pdf_obj pdf_obj_t;
typedef struct pdf_xref_entry pdf_xref_entry_t;
typedef struct pdf_lexer pdf_lexer_t;
typedef struct pdf_cmap pdf_cmap_t;
typedef struct pdf_font pdf_font_t;
typedef struct pdf_gstate pdf_gstate_t;
typedef struct pdf_text_ctx pdf_text_ctx_t;

/* === PDF Object Types === */
typedef enum {
    PDF_OBJ_NULL = 0,
    PDF_OBJ_BOOL,
    PDF_OBJ_INT,
    PDF_OBJ_REAL,
    PDF_OBJ_STRING,
    PDF_OBJ_NAME,
    PDF_OBJ_ARRAY,
    PDF_OBJ_DICT,
    PDF_OBJ_STREAM,
    PDF_OBJ_REF,
} pdf_obj_type_t;

/* === PDF Object === */
struct pdf_obj {
    pdf_obj_type_t type;
    union {
        int boolean;
        int64_t integer;
        double real;
        struct { char *data; int len; } string;  /* literal or hex string */
        char *name;                               /* /Name (without /) */
        struct { pdf_obj_t **items; int count; } array;
        struct { char **keys; pdf_obj_t **vals; int count; } dict;
        struct {
            pdf_obj_t *dict_obj;     /* dictionary part */
            const uint8_t *raw;      /* pointer into source data */
            size_t raw_len;          /* length of raw stream data */
            int objnum;              /* object number (for encryption key derivation) */
            int gen;                 /* generation number */
        } stream;
        struct { int num; int gen; } ref;
    } v;
};

/* === Cross-Reference Entry === */
struct pdf_xref_entry {
    size_t offset;       /* byte offset in file (type 1) or obj num of objstm (type 2) */
    int gen;             /* generation number (type 1) or index in objstm (type 2) */
    int type;            /* 0=free, 1=normal, 2=compressed (in objstm) */
};

/* === Encryption State === */
typedef struct {
    int enabled;
    int v;               /* algorithm version */
    int r;               /* revision */
    int key_len;         /* in bytes */
    uint8_t key[32];     /* encryption key */
    uint8_t o[32];       /* O entry */
    uint8_t u[32];       /* U entry */
    int32_t p;           /* permissions */
    uint8_t file_id[16]; /* first element of /ID array */
    int has_file_id;
    int method;          /* 0=RC4, 1=AES128 */
} pdf_encrypt_t;

/* === ToUnicode CMap === */
struct pdf_cmap {
    uint32_t map[PDF_CMAP_MAX];   /* code → unicode codepoint (supports BMP+) */
    int max_code;                  /* highest mapped code */
    int is_two_byte;              /* 1 if codes are 2-byte */
};

/* === Font State === */
struct pdf_font {
    char name[64];
    pdf_cmap_t *tounicode;          /* ToUnicode CMap (highest priority) */
    const uint16_t *encoding;       /* base encoding table (256 entries) */
    uint16_t differences[256];      /* /Differences overlay */
    int has_differences;
    int is_cid;                     /* CIDFont (Identity-H etc.) */
};

/* === Graphics State === */
struct pdf_gstate {
    float ctm[6];       /* current transformation matrix */
    float tm[6];        /* text matrix */
    float tlm[6];       /* text line matrix */
    float tc;           /* character spacing */
    float tw;           /* word spacing */
    float th;           /* horizontal scaling (percent / 100) */
    float tl;           /* text leading */
    float trise;        /* text rise */
    float font_size;
    pdf_font_t *font;
};

/* === Text Extraction Context === */
struct pdf_text_ctx {
    char *buf;
    size_t len;
    size_t cap;
    pdf_gstate_t gs;
    pdf_gstate_t gs_stack[PDF_MAX_GSTATE_STACK];
    int gs_top;
    float last_x;
    float last_y;
    int had_text;
};

/* === Lexer === */
struct pdf_lexer {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

/* === Document === */
struct pdf_doc {
    const uint8_t *data;
    size_t len;
    pdf_xref_entry_t *xref;
    int xref_count;
    pdf_obj_t *trailer;
    pdf_encrypt_t encrypt;
    /* object cache (lazy, populated on resolve) */
    pdf_obj_t **obj_cache;
};

/* =====================================================================
 * Utility Helpers
 * ===================================================================== */

/* Growable buffer append */
static int buf_append(pdf_text_ctx_t *ctx, const char *s, size_t slen) {
    if (ctx->len + slen >= ctx->cap) {
        size_t newcap = ctx->cap * 2;
        if (newcap < ctx->len + slen + 1) newcap = ctx->len + slen + 1024;
        char *nb = (char *)realloc(ctx->buf, newcap);
        if (!nb) return -1;
        ctx->buf = nb;
        ctx->cap = newcap;
    }
    memcpy(ctx->buf + ctx->len, s, slen);
    ctx->len += slen;
    ctx->buf[ctx->len] = '\0';
    return 0;
}

static int buf_append_char(pdf_text_ctx_t *ctx, char c) {
    return buf_append(ctx, &c, 1);
}

/* Encode a Unicode codepoint as UTF-8 into buffer */
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Append a Unicode codepoint as UTF-8 */
static int buf_append_unicode(pdf_text_ctx_t *ctx, uint32_t cp) {
    char u8[4];
    int n = utf8_encode(cp, u8);
    if (n > 0) return buf_append(ctx, u8, n);
    return 0;
}

/* Skip whitespace in lexer */
static void lex_skip_ws(pdf_lexer_t *lex) {
    while (lex->pos < lex->len) {
        uint8_t c = lex->data[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0') {
            lex->pos++;
        } else if (c == '%') {
            /* comment — skip to EOL */
            while (lex->pos < lex->len && lex->data[lex->pos] != '\n' && lex->data[lex->pos] != '\r')
                lex->pos++;
        } else {
            break;
        }
    }
}

/* Check if byte is PDF whitespace */
static int is_pdf_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

/* Check if byte is PDF delimiter */
static int is_pdf_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

/* Parse hex digit */
static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Simple identity matrix */
static void mat_identity(float m[6]) {
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;
}

/* Matrix multiply: result = a * b */
static void mat_multiply(float r[6], const float a[6], const float b[6]) {
    r[0] = a[0]*b[0] + a[1]*b[2];
    r[1] = a[0]*b[1] + a[1]*b[3];
    r[2] = a[2]*b[0] + a[3]*b[2];
    r[3] = a[2]*b[1] + a[3]*b[3];
    r[4] = a[4]*b[0] + a[5]*b[2] + b[4];
    r[5] = a[4]*b[1] + a[5]*b[3] + b[5];
}

/* Free a PDF object recursively */
static void pdf_obj_free(pdf_obj_t *obj) {
    if (!obj) return;
    switch (obj->type) {
        case PDF_OBJ_STRING:
            free(obj->v.string.data);
            break;
        case PDF_OBJ_NAME:
            free(obj->v.name);
            break;
        case PDF_OBJ_ARRAY:
            for (int i = 0; i < obj->v.array.count; i++)
                pdf_obj_free(obj->v.array.items[i]);
            free(obj->v.array.items);
            break;
        case PDF_OBJ_DICT:
            for (int i = 0; i < obj->v.dict.count; i++) {
                free(obj->v.dict.keys[i]);
                pdf_obj_free(obj->v.dict.vals[i]);
            }
            free(obj->v.dict.keys);
            free(obj->v.dict.vals);
            break;
        case PDF_OBJ_STREAM:
            pdf_obj_free(obj->v.stream.dict_obj);
            break;
        default:
            break;
    }
    free(obj);
}

/* Allocate a PDF object */
static pdf_obj_t *pdf_obj_new(pdf_obj_type_t type) {
    pdf_obj_t *o = (pdf_obj_t *)calloc(1, sizeof(pdf_obj_t));
    if (o) o->type = type;
    return o;
}

/* Dictionary lookup by key name */
static pdf_obj_t *dict_get(pdf_obj_t *dict, const char *key) {
    if (!dict || dict->type != PDF_OBJ_DICT) return NULL;
    for (int i = 0; i < dict->v.dict.count; i++) {
        if (strcmp(dict->v.dict.keys[i], key) == 0)
            return dict->v.dict.vals[i];
    }
    return NULL;
}

/* Get integer from object */
static int64_t obj_int(pdf_obj_t *o) {
    if (!o) return 0;
    if (o->type == PDF_OBJ_INT) return o->v.integer;
    if (o->type == PDF_OBJ_REAL) return (int64_t)o->v.real;
    return 0;
}

/* Get float from object */
static double obj_real(pdf_obj_t *o) {
    if (!o) return 0.0;
    if (o->type == PDF_OBJ_REAL) return o->v.real;
    if (o->type == PDF_OBJ_INT) return (double)o->v.integer;
    return 0.0;
}

/* Get name string from object (without /) */
static const char *obj_name(pdf_obj_t *o) {
    if (!o || o->type != PDF_OBJ_NAME) return NULL;
    return o->v.name;
}

/* Forward declarations for resolve + parse */
static pdf_obj_t *pdf_resolve(pdf_doc_t *doc, pdf_obj_t *obj);
static pdf_obj_t *pdf_parse_object(pdf_lexer_t *lex);
static pdf_obj_t *pdf_parse_indirect(pdf_doc_t *doc, int objnum);
static int pdf_decrypt_string(pdf_doc_t *doc, int objnum, int gen, uint8_t *data, int len, uint8_t **out, int *out_len);
static int pdf_decompress_stream(pdf_doc_t *doc, pdf_obj_t *stm_obj, int objnum, int gen, uint8_t **out, size_t *out_len);

/* =====================================================================
 * Section 2: PDF Lexer / Object Parser
 * ===================================================================== */

/* Parse a PDF literal string (...) with escape handling */
static pdf_obj_t *lex_parse_string(pdf_lexer_t *lex) {
    lex->pos++; /* skip '(' */
    int depth = 1;
    size_t cap = 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int blen = 0;

    while (lex->pos < lex->len && depth > 0) {
        uint8_t c = lex->data[lex->pos];
        if (c == '(') { depth++; buf[blen++] = (char)c; lex->pos++; }
        else if (c == ')') {
            depth--;
            if (depth > 0) buf[blen++] = (char)c;
            lex->pos++;
        } else if (c == '\\') {
            lex->pos++;
            if (lex->pos >= lex->len) break;
            c = lex->data[lex->pos++];
            switch (c) {
                case 'n': buf[blen++] = '\n'; break;
                case 'r': buf[blen++] = '\r'; break;
                case 't': buf[blen++] = '\t'; break;
                case 'b': buf[blen++] = '\b'; break;
                case 'f': buf[blen++] = '\f'; break;
                case '(': buf[blen++] = '('; break;
                case ')': buf[blen++] = ')'; break;
                case '\\': buf[blen++] = '\\'; break;
                case '\r':
                    if (lex->pos < lex->len && lex->data[lex->pos] == '\n') lex->pos++;
                    break; /* line continuation */
                case '\n': break; /* line continuation */
                default:
                    /* octal escape \DDD */
                    if (c >= '0' && c <= '7') {
                        int oct = c - '0';
                        if (lex->pos < lex->len && lex->data[lex->pos] >= '0' && lex->data[lex->pos] <= '7')
                            oct = oct * 8 + (lex->data[lex->pos++] - '0');
                        if (lex->pos < lex->len && lex->data[lex->pos] >= '0' && lex->data[lex->pos] <= '7')
                            oct = oct * 8 + (lex->data[lex->pos++] - '0');
                        buf[blen++] = (char)(oct & 0xFF);
                    } else {
                        buf[blen++] = (char)c;
                    }
                    break;
            }
        } else {
            buf[blen++] = (char)c;
            lex->pos++;
        }
        if ((size_t)blen + 4 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }

    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_STRING);
    if (!o) { free(buf); return NULL; }
    o->v.string.data = buf;
    o->v.string.len = blen;
    return o;
}

/* Parse a hex string <...> */
static pdf_obj_t *lex_parse_hex_string(pdf_lexer_t *lex) {
    lex->pos++; /* skip '<' */
    size_t cap = 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int blen = 0;
    int high = -1;

    while (lex->pos < lex->len) {
        uint8_t c = lex->data[lex->pos];
        if (c == '>') { lex->pos++; break; }
        if (is_pdf_ws(c)) { lex->pos++; continue; }
        int d = hex_digit(c);
        if (d < 0) { lex->pos++; continue; }
        if (high < 0) {
            high = d;
        } else {
            if ((size_t)blen + 2 >= cap) {
                cap *= 2;
                char *nb = (char *)realloc(buf, cap);
                if (!nb) { free(buf); return NULL; }
                buf = nb;
            }
            buf[blen++] = (char)((high << 4) | d);
            high = -1;
        }
        lex->pos++;
    }
    /* trailing nibble — pad with 0 */
    if (high >= 0) buf[blen++] = (char)(high << 4);

    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_STRING);
    if (!o) { free(buf); return NULL; }
    o->v.string.data = buf;
    o->v.string.len = blen;
    return o;
}

/* Parse a name /Something */
static pdf_obj_t *lex_parse_name(pdf_lexer_t *lex) {
    lex->pos++; /* skip '/' */
    char name[256];
    int nlen = 0;
    while (lex->pos < lex->len && nlen < 254) {
        uint8_t c = lex->data[lex->pos];
        if (is_pdf_ws(c) || is_pdf_delim(c)) break;
        if (c == '#' && lex->pos + 2 < lex->len) {
            int h1 = hex_digit(lex->data[lex->pos + 1]);
            int h2 = hex_digit(lex->data[lex->pos + 2]);
            if (h1 >= 0 && h2 >= 0) {
                name[nlen++] = (char)((h1 << 4) | h2);
                lex->pos += 3;
                continue;
            }
        }
        name[nlen++] = (char)c;
        lex->pos++;
    }
    name[nlen] = '\0';

    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_NAME);
    if (!o) return NULL;
    o->v.name = strdup(name);
    return o;
}

/* Parse a number (integer or real) */
static pdf_obj_t *lex_parse_number(pdf_lexer_t *lex) {
    size_t start = lex->pos;
    int has_dot = 0;
    if (lex->data[lex->pos] == '+' || lex->data[lex->pos] == '-') lex->pos++;
    while (lex->pos < lex->len) {
        uint8_t c = lex->data[lex->pos];
        if (c == '.') { has_dot = 1; lex->pos++; }
        else if (c >= '0' && c <= '9') lex->pos++;
        else break;
    }

    char tmp[64];
    size_t tlen = lex->pos - start;
    if (tlen >= sizeof(tmp)) tlen = sizeof(tmp) - 1;
    memcpy(tmp, lex->data + start, tlen);
    tmp[tlen] = '\0';

    pdf_obj_t *o;
    if (has_dot) {
        o = pdf_obj_new(PDF_OBJ_REAL);
        if (o) o->v.real = atof(tmp);
    } else {
        o = pdf_obj_new(PDF_OBJ_INT);
        if (o) o->v.integer = strtoll(tmp, NULL, 10);
    }
    return o;
}

/* Parse a keyword (true, false, null, or stream marker) */
static pdf_obj_t *lex_parse_keyword(pdf_lexer_t *lex) {
    size_t start = lex->pos;
    while (lex->pos < lex->len && !is_pdf_ws(lex->data[lex->pos]) && !is_pdf_delim(lex->data[lex->pos]))
        lex->pos++;

    size_t klen = lex->pos - start;
    if (klen == 4 && memcmp(lex->data + start, "true", 4) == 0) {
        pdf_obj_t *o = pdf_obj_new(PDF_OBJ_BOOL);
        if (o) o->v.boolean = 1;
        return o;
    }
    if (klen == 5 && memcmp(lex->data + start, "false", 5) == 0) {
        pdf_obj_t *o = pdf_obj_new(PDF_OBJ_BOOL);
        if (o) o->v.boolean = 0;
        return o;
    }
    if (klen == 4 && memcmp(lex->data + start, "null", 4) == 0) {
        return pdf_obj_new(PDF_OBJ_NULL);
    }
    /* Unknown keyword — return as name */
    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_NAME);
    if (o) {
        o->v.name = (char *)malloc(klen + 1);
        if (o->v.name) { memcpy(o->v.name, lex->data + start, klen); o->v.name[klen] = '\0'; }
    }
    return o;
}

/* Parse an array [...] */
static pdf_obj_t *lex_parse_array(pdf_lexer_t *lex) {
    lex->pos++; /* skip '[' */
    int cap = 16, cnt = 0;
    pdf_obj_t **items = (pdf_obj_t **)malloc(cap * sizeof(pdf_obj_t *));
    if (!items) return NULL;

    while (1) {
        lex_skip_ws(lex);
        if (lex->pos >= lex->len) break;
        if (lex->data[lex->pos] == ']') { lex->pos++; break; }

        pdf_obj_t *item = pdf_parse_object(lex);
        if (!item) break;
        if (cnt >= cap) {
            cap *= 2;
            pdf_obj_t **ni = (pdf_obj_t **)realloc(items, cap * sizeof(pdf_obj_t *));
            if (!ni) { pdf_obj_free(item); break; }
            items = ni;
        }
        items[cnt++] = item;
    }

    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_ARRAY);
    if (!o) {
        for (int i = 0; i < cnt; i++) pdf_obj_free(items[i]);
        free(items);
        return NULL;
    }
    o->v.array.items = items;
    o->v.array.count = cnt;
    return o;
}

/* Parse a dictionary << ... >> (may also parse stream if followed by 'stream') */
static pdf_obj_t *lex_parse_dict(pdf_lexer_t *lex) {
    lex->pos += 2; /* skip '<<' */
    int cap = 16, cnt = 0;
    char **keys = (char **)malloc(cap * sizeof(char *));
    pdf_obj_t **vals = (pdf_obj_t **)malloc(cap * sizeof(pdf_obj_t *));
    if (!keys || !vals) { free(keys); free(vals); return NULL; }

    while (1) {
        lex_skip_ws(lex);
        if (lex->pos >= lex->len) break;
        if (lex->pos + 1 < lex->len && lex->data[lex->pos] == '>' && lex->data[lex->pos + 1] == '>') {
            lex->pos += 2;
            break;
        }
        /* key must be a name */
        if (lex->data[lex->pos] != '/') break;
        pdf_obj_t *kobj = lex_parse_name(lex);
        if (!kobj || kobj->type != PDF_OBJ_NAME) { pdf_obj_free(kobj); break; }

        lex_skip_ws(lex);
        pdf_obj_t *val = pdf_parse_object(lex);

        if (cnt >= cap) {
            cap *= 2;
            char **nk = (char **)realloc(keys, cap * sizeof(char *));
            pdf_obj_t **nv = (pdf_obj_t **)realloc(vals, cap * sizeof(pdf_obj_t *));
            if (!nk || !nv) { free(nk); free(nv); pdf_obj_free(kobj); pdf_obj_free(val); break; }
            keys = nk; vals = nv;
        }
        keys[cnt] = kobj->v.name;
        kobj->v.name = NULL; /* transfer ownership */
        free(kobj); /* free shell only */
        vals[cnt] = val;
        cnt++;
    }

    pdf_obj_t *o = pdf_obj_new(PDF_OBJ_DICT);
    if (!o) {
        for (int i = 0; i < cnt; i++) { free(keys[i]); pdf_obj_free(vals[i]); }
        free(keys); free(vals);
        return NULL;
    }
    o->v.dict.keys = keys;
    o->v.dict.vals = vals;
    o->v.dict.count = cnt;

    /* Check if followed by 'stream' keyword → convert to stream object */
    size_t save = lex->pos;
    lex_skip_ws(lex);
    if (lex->pos + 6 <= lex->len && memcmp(lex->data + lex->pos, "stream", 6) == 0) {
        lex->pos += 6;
        /* skip stream keyword EOL: \r\n or \n or \r */
        if (lex->pos < lex->len && lex->data[lex->pos] == '\r') lex->pos++;
        if (lex->pos < lex->len && lex->data[lex->pos] == '\n') lex->pos++;

        /* Get /Length — may be an indirect ref (very common in real PDFs) */
        pdf_obj_t *len_obj = dict_get(o, "Length");
        size_t slen = 0;
        if (len_obj) {
            if (len_obj->type == PDF_OBJ_INT) {
                slen = (size_t)len_obj->v.integer;
            } else if (len_obj->type == PDF_OBJ_REAL) {
                slen = (size_t)len_obj->v.real;
            }
            /* If it's a ref or we got 0, we can't resolve it here (no doc context).
             * Fall back to scanning for 'endstream' marker. */
        }

        if (slen == 0) {
            /* Scan for 'endstream' to determine actual length */
            const uint8_t *search = lex->data + lex->pos;
            size_t remaining = lex->len - lex->pos;
            for (size_t si = 0; si + 9 <= remaining; si++) {
                if (memcmp(search + si, "endstream", 9) == 0) {
                    slen = si;
                    /* Trim trailing whitespace before endstream */
                    while (slen > 0 && (search[slen-1] == '\r' || search[slen-1] == '\n'))
                        slen--;
                    break;
                }
            }
        }

        /* Clamp to available data */
        if (lex->pos + slen > lex->len) slen = lex->len - lex->pos;

        const uint8_t *stream_start = lex->data + lex->pos;
        lex->pos += slen;

        /* skip 'endstream' */
        lex_skip_ws(lex);
        if (lex->pos + 9 <= lex->len && memcmp(lex->data + lex->pos, "endstream", 9) == 0)
            lex->pos += 9;

        /* Convert dict → stream */
        pdf_obj_t *stm = pdf_obj_new(PDF_OBJ_STREAM);
        if (stm) {
            stm->v.stream.dict_obj = o;
            stm->v.stream.raw = stream_start;
            stm->v.stream.raw_len = slen;
            return stm;
        }
        /* fallthrough: return as dict if stream alloc fails */
    } else {
        lex->pos = save; /* rewind */
    }

    return o;
}

/* Main object parser — dispatches based on current byte */
static pdf_obj_t *pdf_parse_object(pdf_lexer_t *lex) {
    lex_skip_ws(lex);
    if (lex->pos >= lex->len) return NULL;

    uint8_t c = lex->data[lex->pos];

    /* Dictionary or hex string */
    if (c == '<') {
        if (lex->pos + 1 < lex->len && lex->data[lex->pos + 1] == '<')
            return lex_parse_dict(lex);
        return lex_parse_hex_string(lex);
    }

    /* Array */
    if (c == '[') return lex_parse_array(lex);

    /* Name */
    if (c == '/') return lex_parse_name(lex);

    /* Literal string */
    if (c == '(') return lex_parse_string(lex);

    /* Number — also handle indirect references: N G R */
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        pdf_obj_t *num = lex_parse_number(lex);
        if (!num) return NULL;

        /* Check for 'N G R' pattern (indirect reference) */
        if (num->type == PDF_OBJ_INT) {
            size_t save2 = lex->pos;
            lex_skip_ws(lex);
            if (lex->pos < lex->len && lex->data[lex->pos] >= '0' && lex->data[lex->pos] <= '9') {
                pdf_obj_t *gen = lex_parse_number(lex);
                if (gen && gen->type == PDF_OBJ_INT) {
                    lex_skip_ws(lex);
                    if (lex->pos < lex->len && lex->data[lex->pos] == 'R') {
                        lex->pos++; /* consume 'R' */
                        pdf_obj_t *ref = pdf_obj_new(PDF_OBJ_REF);
                        if (ref) {
                            ref->v.ref.num = (int)num->v.integer;
                            ref->v.ref.gen = (int)gen->v.integer;
                        }
                        pdf_obj_free(num);
                        pdf_obj_free(gen);
                        return ref;
                    }
                }
                pdf_obj_free(gen);
                lex->pos = save2; /* rewind */
            } else {
                lex->pos = save2; /* rewind ws skip */
            }
        }
        return num;
    }

    /* Keyword (true, false, null, endobj, etc.) */
    if (isalpha(c)) return lex_parse_keyword(lex);

    /* Unknown — skip */
    lex->pos++;
    return NULL;
}

/* Parse an indirect object: N G obj ... endobj */
static pdf_obj_t *pdf_parse_indirect_at(pdf_lexer_t *lex) {
    /* We expect: <int> <int> obj <object> endobj */
    lex_skip_ws(lex);

    /* Parse object number and generation */
    int objnum = 0, gen = 0;
    while (lex->pos < lex->len && lex->data[lex->pos] >= '0' && lex->data[lex->pos] <= '9')
        objnum = objnum * 10 + (lex->data[lex->pos++] - '0');
    lex_skip_ws(lex);
    while (lex->pos < lex->len && lex->data[lex->pos] >= '0' && lex->data[lex->pos] <= '9')
        gen = gen * 10 + (lex->data[lex->pos++] - '0');

    /* Skip to 'obj' keyword */
    while (lex->pos + 3 <= lex->len) {
        if (lex->data[lex->pos] == 'o' && lex->data[lex->pos+1] == 'b' && lex->data[lex->pos+2] == 'j') {
            uint8_t after = (lex->pos + 3 < lex->len) ? lex->data[lex->pos + 3] : ' ';
            if (is_pdf_ws(after) || is_pdf_delim(after) || lex->pos + 3 == lex->len) {
                lex->pos += 3;
                break;
            }
        }
        lex->pos++;
    }

    pdf_obj_t *obj = pdf_parse_object(lex);

    /* Store objnum/gen in stream objects for encryption key derivation */
    if (obj && obj->type == PDF_OBJ_STREAM) {
        obj->v.stream.objnum = objnum;
        obj->v.stream.gen = gen;
    }

    /* skip 'endobj' */
    lex_skip_ws(lex);
    if (lex->pos + 6 <= lex->len && memcmp(lex->data + lex->pos, "endobj", 6) == 0)
        lex->pos += 6;

    return obj;
}

/* =====================================================================
 * Section 3: Cross-Reference Table Parser
 * ===================================================================== */

/* Find 'startxref' by scanning backwards from EOF */
static int64_t find_startxref(const uint8_t *data, size_t len) {
    /* Search last 1024 bytes for "startxref" */
    size_t search_start = (len > 1024) ? len - 1024 : 0;
    for (size_t i = len - 9; i > search_start; i--) {
        if (memcmp(data + i, "startxref", 9) == 0) {
            /* parse the number after it */
            size_t p = i + 9;
            while (p < len && is_pdf_ws(data[p])) p++;
            int64_t val = 0;
            while (p < len && data[p] >= '0' && data[p] <= '9') {
                val = val * 10 + (data[p] - '0');
                p++;
            }
            return val;
        }
    }
    return -1;
}

/* Parse traditional xref table at given offset. Returns trailer dict. */
static pdf_obj_t *parse_xref_table(pdf_doc_t *doc, size_t offset) {
    if (offset >= doc->len) return NULL;
    pdf_lexer_t lex = { doc->data, doc->len, offset };

    /* Skip "xref" keyword */
    lex_skip_ws(&lex);
    if (lex.pos + 4 <= lex.len && memcmp(lex.data + lex.pos, "xref", 4) == 0)
        lex.pos += 4;
    else
        return NULL;

    /* Parse subsections: <first> <count> then 20-byte entries */
    while (1) {
        lex_skip_ws(&lex);
        if (lex.pos >= lex.len) break;

        /* Check for "trailer" keyword */
        if (lex.pos + 7 <= lex.len && memcmp(lex.data + lex.pos, "trailer", 7) == 0) {
            lex.pos += 7;
            lex_skip_ws(&lex);
            return pdf_parse_object(&lex);
        }

        /* Parse subsection header: first_obj count */
        int first = 0, count = 0;
        while (lex.pos < lex.len && lex.data[lex.pos] >= '0' && lex.data[lex.pos] <= '9')
            first = first * 10 + (lex.data[lex.pos++] - '0');
        lex_skip_ws(&lex);
        while (lex.pos < lex.len && lex.data[lex.pos] >= '0' && lex.data[lex.pos] <= '9')
            count = count * 10 + (lex.data[lex.pos++] - '0');

        /* Grow xref table if needed */
        int needed = first + count;
        if (needed > PDF_MAX_OBJECTS) needed = PDF_MAX_OBJECTS;
        if (needed > doc->xref_count) {
            pdf_xref_entry_t *nx = (pdf_xref_entry_t *)realloc(doc->xref,
                needed * sizeof(pdf_xref_entry_t));
            if (!nx) return NULL;
            /* Zero new entries */
            memset(nx + doc->xref_count, 0,
                   (needed - doc->xref_count) * sizeof(pdf_xref_entry_t));
            doc->xref = nx;
            doc->xref_count = needed;
        }

        /* Parse 20-byte entries: "OOOOOOOOOO GGGGG T \n" */
        for (int i = 0; i < count && first + i < doc->xref_count; i++) {
            lex_skip_ws(&lex);
            if (lex.pos + 18 > lex.len) break;

            /* offset (10 digits) */
            size_t off = 0;
            for (int j = 0; j < 10 && lex.pos < lex.len; j++) {
                if (lex.data[lex.pos] >= '0' && lex.data[lex.pos] <= '9')
                    off = off * 10 + (lex.data[lex.pos] - '0');
                lex.pos++;
            }
            /* skip space */
            if (lex.pos < lex.len) lex.pos++;
            /* gen (5 digits) */
            int gen = 0;
            for (int j = 0; j < 5 && lex.pos < lex.len; j++) {
                if (lex.data[lex.pos] >= '0' && lex.data[lex.pos] <= '9')
                    gen = gen * 10 + (lex.data[lex.pos] - '0');
                lex.pos++;
            }
            /* skip space + type char */
            if (lex.pos < lex.len) lex.pos++; /* space */
            char type_ch = 'f';
            if (lex.pos < lex.len) { type_ch = (char)lex.data[lex.pos]; lex.pos++; }

            int idx = first + i;
            /* Only set if not already set (first xref wins for incremental updates) */
            if (doc->xref[idx].type == 0 && type_ch == 'n') {
                doc->xref[idx].offset = off;
                doc->xref[idx].gen = gen;
                doc->xref[idx].type = 1;
            }

            /* skip EOL bytes */
            while (lex.pos < lex.len && (lex.data[lex.pos] == '\r' || lex.data[lex.pos] == '\n' || lex.data[lex.pos] == ' '))
                lex.pos++;
        }
    }
    return NULL;
}

/* Forward decl: decompress for xref streams */
static int inflate_data(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/* Parse an xref stream (PDF 1.5+) */
static pdf_obj_t *parse_xref_stream(pdf_doc_t *doc, size_t offset) {
    if (offset >= doc->len) return NULL;
    pdf_lexer_t lex = { doc->data, doc->len, offset };

    /* Parse the indirect object at this offset */
    pdf_obj_t *obj = pdf_parse_indirect_at(&lex);
    if (!obj || obj->type != PDF_OBJ_STREAM) { pdf_obj_free(obj); return NULL; }

    pdf_obj_t *dict = obj->v.stream.dict_obj;
    pdf_obj_t *type_obj = dict_get(dict, "Type");
    if (!type_obj || !obj_name(type_obj) || strcmp(obj_name(type_obj), "XRef") != 0) {
        pdf_obj_free(obj);
        return NULL;
    }

    /* Decompress the stream */
    uint8_t *stream_data = NULL;
    size_t stream_len = 0;

    pdf_obj_t *filter = dict_get(dict, "Filter");
    if (filter && filter->type == PDF_OBJ_NAME && strcmp(filter->v.name, "FlateDecode") == 0) {
        if (inflate_data(obj->v.stream.raw, obj->v.stream.raw_len, &stream_data, &stream_len) != 0) {
            pdf_obj_free(obj);
            return NULL;
        }
    } else {
        /* No filter or unsupported — use raw */
        stream_len = obj->v.stream.raw_len;
        stream_data = (uint8_t *)malloc(stream_len);
        if (stream_data) memcpy(stream_data, obj->v.stream.raw, stream_len);
    }

    if (!stream_data) { pdf_obj_free(obj); return NULL; }

    /* Parse /W array (field widths) */
    pdf_obj_t *w_obj = dict_get(dict, "W");
    int w[3] = {1, 2, 1}; /* defaults */
    if (w_obj && w_obj->type == PDF_OBJ_ARRAY && w_obj->v.array.count >= 3) {
        w[0] = (int)obj_int(w_obj->v.array.items[0]);
        w[1] = (int)obj_int(w_obj->v.array.items[1]);
        w[2] = (int)obj_int(w_obj->v.array.items[2]);
    }
    int entry_size = w[0] + w[1] + w[2];
    if (entry_size <= 0) { free(stream_data); pdf_obj_free(obj); return NULL; }

    /* Parse /Size */
    int size = (int)obj_int(dict_get(dict, "Size"));
    if (size > PDF_MAX_OBJECTS) size = PDF_MAX_OBJECTS;

    /* Grow xref */
    if (size > doc->xref_count) {
        pdf_xref_entry_t *nx = (pdf_xref_entry_t *)realloc(doc->xref,
            size * sizeof(pdf_xref_entry_t));
        if (!nx) { free(stream_data); pdf_obj_free(obj); return NULL; }
        memset(nx + doc->xref_count, 0,
               (size - doc->xref_count) * sizeof(pdf_xref_entry_t));
        doc->xref = nx;
        doc->xref_count = size;
    }

    /* Parse /Index array (subsection ranges) — default [0 Size] */
    pdf_obj_t *index_obj = dict_get(dict, "Index");
    int *ranges = NULL;
    int nranges = 0;
    if (index_obj && index_obj->type == PDF_OBJ_ARRAY && index_obj->v.array.count >= 2) {
        nranges = index_obj->v.array.count / 2;
        ranges = (int *)malloc(nranges * 2 * sizeof(int));
        for (int i = 0; i < nranges * 2; i++)
            ranges[i] = (int)obj_int(index_obj->v.array.items[i]);
    } else {
        nranges = 1;
        ranges = (int *)malloc(2 * sizeof(int));
        ranges[0] = 0; ranges[1] = size;
    }

    /* Read entries */
    size_t p = 0;
    for (int r = 0; r < nranges; r++) {
        int first = ranges[r * 2];
        int count = ranges[r * 2 + 1];
        for (int i = 0; i < count; i++) {
            if (p + (size_t)entry_size > stream_len) break;
            int idx = first + i;
            if (idx < 0 || idx >= doc->xref_count) { p += entry_size; continue; }

            /* Read field values (big-endian) */
            int64_t fields[3] = {0, 0, 0};
            size_t fp = p;
            for (int f = 0; f < 3; f++) {
                for (int b = 0; b < w[f]; b++) {
                    fields[f] = (fields[f] << 8) | stream_data[fp++];
                }
            }
            p += entry_size;

            /* Default type field to 1 if w[0]==0 */
            int ftype = (w[0] == 0) ? 1 : (int)fields[0];

            if (doc->xref[idx].type != 0) continue; /* first xref wins */

            if (ftype == 1) {
                doc->xref[idx].type = 1;
                doc->xref[idx].offset = (size_t)fields[1];
                doc->xref[idx].gen = (int)fields[2];
            } else if (ftype == 2) {
                doc->xref[idx].type = 2;
                doc->xref[idx].offset = (size_t)fields[1]; /* obj num of ObjStm */
                doc->xref[idx].gen = (int)fields[2];        /* index within ObjStm */
            }
            /* type 0 = free, skip */
        }
    }

    free(ranges);
    free(stream_data);

    /* Build trailer dict from the xref stream dict (it doubles as trailer) */
    /* Clone the dict so we can free obj independently */
    pdf_obj_t *trailer = pdf_obj_new(PDF_OBJ_DICT);
    if (trailer) {
        int dc = dict->v.dict.count;
        trailer->v.dict.keys = (char **)malloc(dc * sizeof(char *));
        trailer->v.dict.vals = (pdf_obj_t **)malloc(dc * sizeof(pdf_obj_t *));
        trailer->v.dict.count = dc;
        /* Shallow copy: steal keys/vals from dict, null out dict */
        for (int i = 0; i < dc; i++) {
            trailer->v.dict.keys[i] = dict->v.dict.keys[i];
            trailer->v.dict.vals[i] = dict->v.dict.vals[i];
            dict->v.dict.keys[i] = NULL;
            dict->v.dict.vals[i] = NULL;
        }
        dict->v.dict.count = 0;
    }

    pdf_obj_free(obj);
    return trailer;
}

/* Load xref tables (following /Prev chain for incremental updates) */
static int load_xref(pdf_doc_t *doc) {
    int64_t xref_offset = find_startxref(doc->data, doc->len);
    if (xref_offset < 0) return SW_PDF_ERR_PARSE;

    int iterations = 0;
    while (xref_offset >= 0 && xref_offset < (int64_t)doc->len && iterations < 32) {
        iterations++;
        pdf_obj_t *trailer = NULL;

        /* Check if it's a traditional xref table or xref stream */
        size_t off = (size_t)xref_offset;
        /* Skip whitespace to see what's there */
        while (off < doc->len && is_pdf_ws(doc->data[off])) off++;

        if (off + 4 <= doc->len && memcmp(doc->data + off, "xref", 4) == 0) {
            trailer = parse_xref_table(doc, off);
        } else {
            /* xref stream — offset points to an indirect object */
            trailer = parse_xref_stream(doc, off);
        }

        if (!trailer) break;

        /* Save first trailer as the document trailer */
        if (!doc->trailer) {
            doc->trailer = trailer;
        }

        /* Follow /Prev chain — check current trailer before freeing */
        pdf_obj_t *prev = dict_get(trailer, "Prev");
        int64_t prev_off = -1;
        if (prev && prev->type == PDF_OBJ_INT) {
            prev_off = prev->v.integer;
        }

        /* Free non-primary trailers */
        if (trailer != doc->trailer) {
            pdf_obj_free(trailer);
        }

        if (prev_off >= 0 && prev_off != xref_offset) {
            xref_offset = prev_off;
        } else {
            break;
        }
    }

    if (!doc->trailer || doc->xref_count == 0) return SW_PDF_ERR_PARSE;
    return SW_PDF_OK;
}

/* Parse an indirect object from the file */
static pdf_obj_t *pdf_parse_indirect(pdf_doc_t *doc, int objnum) {
    if (objnum < 0 || objnum >= doc->xref_count) return NULL;
    if (doc->xref[objnum].type == 0) return NULL;

    /* Check cache */
    if (doc->obj_cache && doc->obj_cache[objnum])
        return doc->obj_cache[objnum];

    pdf_obj_t *result = NULL;

    if (doc->xref[objnum].type == 1) {
        /* Normal object at file offset */
        size_t offset = doc->xref[objnum].offset;
        if (offset >= doc->len) return NULL;
        pdf_lexer_t lex = { doc->data, doc->len, offset };
        result = pdf_parse_indirect_at(&lex);
    } else if (doc->xref[objnum].type == 2) {
        /* Compressed object in ObjStm */
        int stm_num = (int)doc->xref[objnum].offset;
        int stm_idx = doc->xref[objnum].gen;

        /* Get the object stream (recursive — but ObjStm itself must be type 1) */
        pdf_obj_t *stm_obj = pdf_parse_indirect(doc, stm_num);
        if (!stm_obj || stm_obj->type != PDF_OBJ_STREAM) return NULL;

        /* Decompress the object stream */
        uint8_t *stm_data = NULL;
        size_t stm_len = 0;
        if (pdf_decompress_stream(doc, stm_obj, stm_num, 0, &stm_data, &stm_len) != 0)
            return NULL;

        /* Parse header: N pairs of (objnum offset) */
        pdf_obj_t *n_obj = dict_get(stm_obj->v.stream.dict_obj, "N");
        int n = n_obj ? (int)obj_int(n_obj) : 0;
        pdf_obj_t *first_obj = dict_get(stm_obj->v.stream.dict_obj, "First");
        int first = first_obj ? (int)obj_int(first_obj) : 0;

        if (stm_idx < n && first > 0 && (size_t)first <= stm_len) {
            /* Parse the header to find offset of our object */
            pdf_lexer_t hlex = { stm_data, stm_len, 0 };
            int target_offset = -1;
            for (int i = 0; i <= stm_idx && i < n; i++) {
                lex_skip_ws(&hlex);
                /* skip objnum */
                while (hlex.pos < hlex.len && hlex.data[hlex.pos] >= '0' && hlex.data[hlex.pos] <= '9')
                    hlex.pos++;
                lex_skip_ws(&hlex);
                /* read offset */
                int off = 0;
                while (hlex.pos < hlex.len && hlex.data[hlex.pos] >= '0' && hlex.data[hlex.pos] <= '9')
                    off = off * 10 + (hlex.data[hlex.pos++] - '0');
                if (i == stm_idx) target_offset = first + off;
            }

            if (target_offset >= 0 && (size_t)target_offset < stm_len) {
                pdf_lexer_t olex = { stm_data, stm_len, (size_t)target_offset };
                result = pdf_parse_object(&olex);
            }
        }
        free(stm_data);
    }

    /* Cache result */
    if (result && doc->obj_cache && objnum < doc->xref_count) {
        doc->obj_cache[objnum] = result;
    }

    return result;
}

/* Resolve an indirect reference to its actual object */
static pdf_obj_t *pdf_resolve(pdf_doc_t *doc, pdf_obj_t *obj) {
    int depth = 0;
    while (obj && obj->type == PDF_OBJ_REF && depth < PDF_MAX_DEPTH) {
        obj = pdf_parse_indirect(doc, obj->v.ref.num);
        depth++;
    }
    return obj;
}

/* =====================================================================
 * Section 4: Stream Decompressors
 * ===================================================================== */

/* FlateDecode — zlib inflate with auto-detect and fallback */
static int inflate_data(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    size_t cap = in_len * 4;
    if (cap < 4096) cap = 4096;
    if (cap > PDF_MAX_STREAM) cap = PDF_MAX_STREAM;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return SW_PDF_ERR_ALLOC;

    /* Try three modes: auto-detect (zlib+gzip+raw), zlib-only, raw deflate */
    int wbits_modes[] = { 15 + 32, 15, -15 };
    int nmodes = 3;

    for (int mode = 0; mode < nmodes; mode++) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in = (Bytef *)in;
        zs.avail_in = (uInt)in_len;
        zs.next_out = buf;
        zs.avail_out = (uInt)cap;

        if (inflateInit2(&zs, wbits_modes[mode]) != Z_OK) continue;

        int rc;
        int failed = 0;
        while ((rc = inflate(&zs, Z_NO_FLUSH)) != Z_STREAM_END) {
            if (rc == Z_OK && zs.avail_out == 0) {
                /* grow buffer */
                size_t newcap = cap * 2;
                if (newcap > PDF_MAX_STREAM) { failed = 1; break; }
                uint8_t *nb = (uint8_t *)realloc(buf, newcap);
                if (!nb) { inflateEnd(&zs); free(buf); return SW_PDF_ERR_ALLOC; }
                buf = nb;
                zs.next_out = buf + cap;
                zs.avail_out = (uInt)(newcap - cap);
                cap = newcap;
            } else if (rc != Z_OK) {
                failed = 1;
                break;
            }
        }

        size_t produced = zs.total_out;
        inflateEnd(&zs);

        if (!failed && rc == Z_STREAM_END && produced > 0) {
            /* Success — clean decompression */
            *out = buf;
            *out_len = produced;
            return SW_PDF_OK;
        }

        if (failed && produced > 0) {
            /* Partial success — truncated stream, return what we got */
            *out = buf;
            *out_len = produced;
            return SW_PDF_OK;
        }
        /* Try next mode */
    }

    free(buf);
    return SW_PDF_ERR_ZLIB;
}

/* ASCIIHexDecode */
static int ascii_hex_decode(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    uint8_t *buf = (uint8_t *)malloc(in_len / 2 + 1);
    if (!buf) return SW_PDF_ERR_ALLOC;
    size_t blen = 0;
    int high = -1;

    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '>') break;
        int d = hex_digit(in[i]);
        if (d < 0) continue;
        if (high < 0) { high = d; }
        else { buf[blen++] = (uint8_t)((high << 4) | d); high = -1; }
    }
    if (high >= 0) buf[blen++] = (uint8_t)(high << 4);

    *out = buf;
    *out_len = blen;
    return SW_PDF_OK;
}

/* ASCII85Decode */
static int ascii85_decode(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    size_t cap = in_len;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return SW_PDF_ERR_ALLOC;
    size_t blen = 0;

    uint32_t tuple = 0;
    int count = 0;

    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        if (c == '~' && i + 1 < in_len && in[i + 1] == '>') break; /* EOD */
        if (c < '!' && c != 'z') continue; /* whitespace */

        if (c == 'z') {
            /* special: z = 0x00000000 */
            if (blen + 4 > cap) { cap *= 2; buf = (uint8_t *)realloc(buf, cap); if (!buf) return SW_PDF_ERR_ALLOC; }
            buf[blen++] = 0; buf[blen++] = 0; buf[blen++] = 0; buf[blen++] = 0;
            continue;
        }

        tuple = tuple * 85 + (c - 33);
        count++;
        if (count == 5) {
            if (blen + 4 > cap) { cap *= 2; buf = (uint8_t *)realloc(buf, cap); if (!buf) return SW_PDF_ERR_ALLOC; }
            buf[blen++] = (uint8_t)(tuple >> 24);
            buf[blen++] = (uint8_t)(tuple >> 16);
            buf[blen++] = (uint8_t)(tuple >> 8);
            buf[blen++] = (uint8_t)(tuple);
            tuple = 0;
            count = 0;
        }
    }

    /* Handle remainder */
    if (count > 1) {
        for (int i = count; i < 5; i++) tuple = tuple * 85 + 84;
        if (blen + (size_t)(count - 1) > cap) { cap += 4; buf = (uint8_t *)realloc(buf, cap); if (!buf) return SW_PDF_ERR_ALLOC; }
        if (count >= 2) buf[blen++] = (uint8_t)(tuple >> 24);
        if (count >= 3) buf[blen++] = (uint8_t)(tuple >> 16);
        if (count >= 4) buf[blen++] = (uint8_t)(tuple >> 8);
    }

    *out = buf;
    *out_len = blen;
    return SW_PDF_OK;
}

/* LZWDecode */
static int lzw_decode(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    #define LZW_MAX_TABLE 4096
    #define LZW_CLEAR     256
    #define LZW_EOD       257

    typedef struct { uint8_t *data; int len; } lzw_entry_t;
    lzw_entry_t table[LZW_MAX_TABLE];
    uint8_t init_data[256];

    /* Initialize table with single-byte entries */
    for (int i = 0; i < 256; i++) { init_data[i] = (uint8_t)i; table[i].data = &init_data[i]; table[i].len = 1; }
    for (int i = 256; i < LZW_MAX_TABLE; i++) { table[i].data = NULL; table[i].len = 0; }
    int next_code = 258;
    int code_size = 9;

    size_t cap = in_len * 4;
    if (cap < 4096) cap = 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return SW_PDF_ERR_ALLOC;
    size_t blen = 0;

    /* Dynamic string table storage */
    uint8_t *str_pool = (uint8_t *)malloc(LZW_MAX_TABLE * 256);
    if (!str_pool) { free(buf); return SW_PDF_ERR_ALLOC; }
    size_t pool_pos = 0;

    /* Bit reader state */
    uint32_t bit_buf = 0;
    int bits_in = 0;
    size_t byte_pos = 0;

    int prev_code = -1;

    while (1) {
        /* Read next code */
        while (bits_in < code_size && byte_pos < in_len) {
            bit_buf = (bit_buf << 8) | in[byte_pos++];
            bits_in += 8;
        }
        if (bits_in < code_size) break;

        int code = (int)((bit_buf >> (bits_in - code_size)) & ((1 << code_size) - 1));
        bits_in -= code_size;

        if (code == LZW_EOD) break;

        if (code == LZW_CLEAR) {
            /* Reset table */
            for (int i = 258; i < next_code && i < LZW_MAX_TABLE; i++) { table[i].data = NULL; table[i].len = 0; }
            next_code = 258;
            code_size = 9;
            pool_pos = 0;
            prev_code = -1;
            continue;
        }

        uint8_t *entry_data;
        int entry_len;

        if (code < next_code && table[code].data) {
            entry_data = table[code].data;
            entry_len = table[code].len;
        } else if (code == next_code && prev_code >= 0 && table[prev_code].data) {
            /* Special case: code not yet in table */
            if (pool_pos + (size_t)table[prev_code].len + 1 < (size_t)LZW_MAX_TABLE * 256) {
                entry_data = str_pool + pool_pos;
                memcpy(entry_data, table[prev_code].data, table[prev_code].len);
                entry_data[table[prev_code].len] = table[prev_code].data[0];
                entry_len = table[prev_code].len + 1;
                pool_pos += entry_len;
            } else break;
        } else {
            break; /* invalid code */
        }

        /* Output */
        if (blen + (size_t)entry_len > cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) { free(buf); free(str_pool); return SW_PDF_ERR_ALLOC; }
            buf = nb;
        }
        memcpy(buf + blen, entry_data, entry_len);
        blen += entry_len;

        /* Add to table */
        if (prev_code >= 0 && next_code < LZW_MAX_TABLE && table[prev_code].data) {
            int new_len = table[prev_code].len + 1;
            if (pool_pos + (size_t)new_len < (size_t)LZW_MAX_TABLE * 256) {
                uint8_t *new_data = str_pool + pool_pos;
                memcpy(new_data, table[prev_code].data, table[prev_code].len);
                new_data[table[prev_code].len] = entry_data[0];
                table[next_code].data = new_data;
                table[next_code].len = new_len;
                pool_pos += new_len;
                next_code++;
                if (next_code >= (1 << code_size) && code_size < 12)
                    code_size++;
            }
        }

        prev_code = code;
    }

    free(str_pool);
    *out = buf;
    *out_len = blen;
    return SW_PDF_OK;

    #undef LZW_MAX_TABLE
    #undef LZW_CLEAR
    #undef LZW_EOD
}

/* RunLengthDecode */
static int rle_decode(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    size_t cap = in_len * 2;
    if (cap < 4096) cap = 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return SW_PDF_ERR_ALLOC;
    size_t blen = 0, i = 0;

    while (i < in_len) {
        uint8_t b = in[i++];
        if (b == 128) break; /* EOD */
        if (b < 128) {
            /* copy next b+1 bytes */
            int count = b + 1;
            if (blen + count > cap) { cap = (blen + count) * 2; buf = (uint8_t *)realloc(buf, cap); if (!buf) return SW_PDF_ERR_ALLOC; }
            for (int j = 0; j < count && i < in_len; j++)
                buf[blen++] = in[i++];
        } else {
            /* repeat next byte (257-b) times */
            int count = 257 - b;
            if (i >= in_len) break;
            uint8_t val = in[i++];
            if (blen + count > cap) { cap = (blen + count) * 2; buf = (uint8_t *)realloc(buf, cap); if (!buf) return SW_PDF_ERR_ALLOC; }
            memset(buf + blen, val, count);
            blen += count;
        }
    }

    *out = buf;
    *out_len = blen;
    return SW_PDF_OK;
}

/* PNG predictor: undo PNG filtering */
static int apply_png_predictor(uint8_t *data, size_t len, int columns, int colors, int bpc) {
    int bytes_per_pixel = (colors * bpc + 7) / 8;
    int row_bytes = (columns * colors * bpc + 7) / 8;
    int stride = row_bytes + 1; /* +1 for filter byte */

    if (stride <= 0 || (size_t)stride > len) return 0;

    size_t nrows = len / stride;
    uint8_t *prev_row = (uint8_t *)calloc(row_bytes, 1);
    if (!prev_row) return -1;

    size_t out_pos = 0;
    for (size_t r = 0; r < nrows; r++) {
        uint8_t *row = data + r * stride;
        uint8_t filter = row[0];
        uint8_t *pixels = row + 1;

        switch (filter) {
            case 0: /* None */
                break;
            case 1: /* Sub */
                for (int i = bytes_per_pixel; i < row_bytes; i++)
                    pixels[i] = (uint8_t)(pixels[i] + pixels[i - bytes_per_pixel]);
                break;
            case 2: /* Up */
                for (int i = 0; i < row_bytes; i++)
                    pixels[i] = (uint8_t)(pixels[i] + prev_row[i]);
                break;
            case 3: /* Average */
                for (int i = 0; i < row_bytes; i++) {
                    int a = (i >= bytes_per_pixel) ? pixels[i - bytes_per_pixel] : 0;
                    int b = prev_row[i];
                    pixels[i] = (uint8_t)(pixels[i] + (a + b) / 2);
                }
                break;
            case 4: /* Paeth */
                for (int i = 0; i < row_bytes; i++) {
                    int a = (i >= bytes_per_pixel) ? pixels[i - bytes_per_pixel] : 0;
                    int b = prev_row[i];
                    int c = (i >= bytes_per_pixel) ? prev_row[i - bytes_per_pixel] : 0;
                    int p = a + b - c;
                    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    pixels[i] = (uint8_t)(pixels[i] + pr);
                }
                break;
        }

        memcpy(prev_row, pixels, row_bytes);
        /* Compact: remove filter byte */
        memmove(data + out_pos, pixels, row_bytes);
        out_pos += row_bytes;
    }

    free(prev_row);
    return (int)out_pos; /* return compacted size */
}

/* TIFF predictor 2: horizontal differencing */
static void apply_tiff_predictor(uint8_t *data, size_t len, int columns, int colors) {
    int row_bytes = columns * colors;
    if (row_bytes <= 0) return;
    size_t nrows = len / row_bytes;

    for (size_t r = 0; r < nrows; r++) {
        uint8_t *row = data + r * row_bytes;
        for (int i = colors; i < row_bytes; i++)
            row[i] = (uint8_t)(row[i] + row[i - colors]);
    }
}

/* Apply predictor based on /DecodeParms */
static int apply_predictor(uint8_t **data, size_t *len, pdf_obj_t *parms) {
    if (!parms) return 0;
    pdf_obj_t *pred_obj = dict_get(parms, "Predictor");
    int predictor = pred_obj ? (int)obj_int(pred_obj) : 1;
    if (predictor <= 1) return 0; /* no predictor */

    int columns = 1, colors = 1, bpc = 8;
    pdf_obj_t *o;
    if ((o = dict_get(parms, "Columns"))) columns = (int)obj_int(o);
    if ((o = dict_get(parms, "Colors"))) colors = (int)obj_int(o);
    if ((o = dict_get(parms, "BitsPerComponent"))) bpc = (int)obj_int(o);

    if (predictor == 2) {
        apply_tiff_predictor(*data, *len, columns, colors);
    } else if (predictor >= 10 && predictor <= 15) {
        int new_len = apply_png_predictor(*data, *len, columns, colors, bpc);
        if (new_len > 0) *len = (size_t)new_len;
    }
    return 0;
}

/* Decompress a stream object, applying all filters in sequence */
static int pdf_decompress_stream(pdf_doc_t *doc, pdf_obj_t *stm_obj,
                                  int objnum, int gen,
                                  uint8_t **out, size_t *out_len) {
    if (!stm_obj || stm_obj->type != PDF_OBJ_STREAM) return SW_PDF_ERR_PARSE;

    pdf_obj_t *dict = stm_obj->v.stream.dict_obj;
    const uint8_t *raw = stm_obj->v.stream.raw;
    size_t raw_len = stm_obj->v.stream.raw_len;

    /* Decrypt stream if encryption is active */
    uint8_t *decrypted = NULL;
    if (doc->encrypt.enabled) {
        int dec_len = 0;
        if (pdf_decrypt_string(doc, objnum, gen, (uint8_t *)raw, (int)raw_len, &decrypted, &dec_len) == 0) {
            raw = decrypted;
            raw_len = (size_t)dec_len;
        }
    }

    /* Get filter(s) — resolve indirect refs */
    pdf_obj_t *filter = pdf_resolve(doc, dict_get(dict, "Filter"));
    pdf_obj_t *parms = pdf_resolve(doc, dict_get(dict, "DecodeParms"));

    /* Normalize to arrays */
    const char *filters[PDF_MAX_FILTERS];
    pdf_obj_t *parms_arr[PDF_MAX_FILTERS];
    int nfilters = 0;

    if (!filter) {
        /* No filter — raw data */
        *out = (uint8_t *)malloc(raw_len + 1);
        if (!*out) { free(decrypted); return SW_PDF_ERR_ALLOC; }
        memcpy(*out, raw, raw_len);
        *out_len = raw_len;
        free(decrypted);
        return SW_PDF_OK;
    } else if (filter->type == PDF_OBJ_NAME) {
        filters[0] = filter->v.name;
        parms_arr[0] = parms;
        nfilters = 1;
    } else if (filter->type == PDF_OBJ_ARRAY) {
        nfilters = filter->v.array.count;
        if (nfilters > PDF_MAX_FILTERS) nfilters = PDF_MAX_FILTERS;
        for (int i = 0; i < nfilters; i++) {
            pdf_obj_t *fi = filter->v.array.items[i];
            filters[i] = (fi && fi->type == PDF_OBJ_NAME) ? fi->v.name : "";
            parms_arr[i] = (parms && parms->type == PDF_OBJ_ARRAY && i < parms->v.array.count)
                            ? parms->v.array.items[i] : NULL;
        }
    } else {
        nfilters = 0;
    }

    /* Apply filters in sequence */
    uint8_t *cur = (uint8_t *)malloc(raw_len);
    if (!cur) { free(decrypted); return SW_PDF_ERR_ALLOC; }
    memcpy(cur, raw, raw_len);
    size_t cur_len = raw_len;
    free(decrypted);

    for (int i = 0; i < nfilters; i++) {
        uint8_t *next = NULL;
        size_t next_len = 0;
        int rc;

        if (strcmp(filters[i], "FlateDecode") == 0) {
            rc = inflate_data(cur, cur_len, &next, &next_len);
        } else if (strcmp(filters[i], "ASCIIHexDecode") == 0) {
            rc = ascii_hex_decode(cur, cur_len, &next, &next_len);
        } else if (strcmp(filters[i], "ASCII85Decode") == 0) {
            rc = ascii85_decode(cur, cur_len, &next, &next_len);
        } else if (strcmp(filters[i], "LZWDecode") == 0) {
            rc = lzw_decode(cur, cur_len, &next, &next_len);
        } else if (strcmp(filters[i], "RunLengthDecode") == 0) {
            rc = rle_decode(cur, cur_len, &next, &next_len);
        } else {
            /* Unsupported filter — pass through */
            next = cur;
            next_len = cur_len;
            cur = NULL;
            rc = 0;
        }

        if (rc != 0) { free(cur); return rc; }
        free(cur);
        cur = next;
        cur_len = next_len;

        /* Apply predictor */
        pdf_obj_t *p = parms_arr[i];
        if (p) {
            p = pdf_resolve(doc, p);
            apply_predictor(&cur, &cur_len, p);
        }
    }

    *out = cur;
    *out_len = cur_len;
    return SW_PDF_OK;
}

/* =====================================================================
 * Section 5: Encryption Handler (RC4, AES-128-CBC)
 * ===================================================================== */

/* Simple MD5 implementation (for key derivation — PDF encryption uses MD5) */
typedef struct { uint32_t s[4]; uint8_t buf[64]; uint32_t count[2]; } md5_ctx_t;

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);

    #define F(x,y,z) (((x)&(y))|((~(x))&(z)))
    #define G(x,y,z) (((x)&(z))|((y)&(~(z))))
    #define H(x,y,z) ((x)^(y)^(z))
    #define I(x,y,z) ((y)^((x)|(~(z))))
    #define ROT(x,n) (((x)<<(n))|((x)>>(32-(n))))
    #define STEP(f,a,b,c,d,x,t,s) (a)+=f((b),(c),(d))+(x)+(t);(a)=ROT((a),(s));(a)+=(b)

    STEP(F,a,b,c,d,M[ 0],0xd76aa478, 7); STEP(F,d,a,b,c,M[ 1],0xe8c7b756,12);
    STEP(F,c,d,a,b,M[ 2],0x242070db,17); STEP(F,b,c,d,a,M[ 3],0xc1bdceee,22);
    STEP(F,a,b,c,d,M[ 4],0xf57c0faf, 7); STEP(F,d,a,b,c,M[ 5],0x4787c62a,12);
    STEP(F,c,d,a,b,M[ 6],0xa8304613,17); STEP(F,b,c,d,a,M[ 7],0xfd469501,22);
    STEP(F,a,b,c,d,M[ 8],0x698098d8, 7); STEP(F,d,a,b,c,M[ 9],0x8b44f7af,12);
    STEP(F,c,d,a,b,M[10],0xffff5bb1,17); STEP(F,b,c,d,a,M[11],0x895cd7be,22);
    STEP(F,a,b,c,d,M[12],0x6b901122, 7); STEP(F,d,a,b,c,M[13],0xfd987193,12);
    STEP(F,c,d,a,b,M[14],0xa679438e,17); STEP(F,b,c,d,a,M[15],0x49b40821,22);

    STEP(G,a,b,c,d,M[ 1],0xf61e2562, 5); STEP(G,d,a,b,c,M[ 6],0xc040b340, 9);
    STEP(G,c,d,a,b,M[11],0x265e5a51,14); STEP(G,b,c,d,a,M[ 0],0xe9b6c7aa,20);
    STEP(G,a,b,c,d,M[ 5],0xd62f105d, 5); STEP(G,d,a,b,c,M[10],0x02441453, 9);
    STEP(G,c,d,a,b,M[15],0xd8a1e681,14); STEP(G,b,c,d,a,M[ 4],0xe7d3fbc8,20);
    STEP(G,a,b,c,d,M[ 9],0x21e1cde6, 5); STEP(G,d,a,b,c,M[14],0xc33707d6, 9);
    STEP(G,c,d,a,b,M[ 3],0xf4d50d87,14); STEP(G,b,c,d,a,M[ 8],0x455a14ed,20);
    STEP(G,a,b,c,d,M[13],0xa9e3e905, 5); STEP(G,d,a,b,c,M[ 2],0xfcefa3f8, 9);
    STEP(G,c,d,a,b,M[ 7],0x676f02d9,14); STEP(G,b,c,d,a,M[12],0x8d2a4c8a,20);

    STEP(H,a,b,c,d,M[ 5],0xfffa3942, 4); STEP(H,d,a,b,c,M[ 8],0x8771f681,11);
    STEP(H,c,d,a,b,M[11],0x6d9d6122,16); STEP(H,b,c,d,a,M[14],0xfde5380c,23);
    STEP(H,a,b,c,d,M[ 1],0xa4beea44, 4); STEP(H,d,a,b,c,M[ 4],0x4bdecfa9,11);
    STEP(H,c,d,a,b,M[ 7],0xf6bb4b60,16); STEP(H,b,c,d,a,M[10],0xbebfbc70,23);
    STEP(H,a,b,c,d,M[13],0x289b7ec6, 4); STEP(H,d,a,b,c,M[ 0],0xeaa127fa,11);
    STEP(H,c,d,a,b,M[ 3],0xd4ef3085,16); STEP(H,b,c,d,a,M[ 6],0x04881d05,23);
    STEP(H,a,b,c,d,M[ 9],0xd9d4d039, 4); STEP(H,d,a,b,c,M[12],0xe6db99e5,11);
    STEP(H,c,d,a,b,M[15],0x1fa27cf8,16); STEP(H,b,c,d,a,M[ 2],0xc4ac5665,23);

    STEP(I,a,b,c,d,M[ 0],0xf4292244, 6); STEP(I,d,a,b,c,M[ 7],0x432aff97,10);
    STEP(I,c,d,a,b,M[14],0xab9423a7,15); STEP(I,b,c,d,a,M[ 5],0xfc93a039,21);
    STEP(I,a,b,c,d,M[12],0x655b59c3, 6); STEP(I,d,a,b,c,M[ 3],0x8f0ccc92,10);
    STEP(I,c,d,a,b,M[10],0xffeff47d,15); STEP(I,b,c,d,a,M[ 1],0x85845dd1,21);
    STEP(I,a,b,c,d,M[ 8],0x6fa87e4f, 6); STEP(I,d,a,b,c,M[15],0xfe2ce6e0,10);
    STEP(I,c,d,a,b,M[ 6],0xa3014314,15); STEP(I,b,c,d,a,M[13],0x4e0811a1,21);
    STEP(I,a,b,c,d,M[ 4],0xf7537e82, 6); STEP(I,d,a,b,c,M[11],0xbd3af235,10);
    STEP(I,c,d,a,b,M[ 2],0x2ad7d2bb,15); STEP(I,b,c,d,a,M[ 9],0xeb86d391,21);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;

    #undef F
    #undef G
    #undef H
    #undef I
    #undef ROT
    #undef STEP
}

static void md5_init(md5_ctx_t *ctx) {
    ctx->s[0] = 0x67452301; ctx->s[1] = 0xefcdab89;
    ctx->s[2] = 0x98badcfe; ctx->s[3] = 0x10325476;
    ctx->count[0] = ctx->count[1] = 0;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len) {
    uint32_t idx = (ctx->count[0] >> 3) & 63;
    ctx->count[0] += (uint32_t)(len << 3);
    if (ctx->count[0] < (uint32_t)(len << 3)) ctx->count[1]++;
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t i = 0;
    if (idx + len >= 64) {
        size_t partial = 64 - idx;
        memcpy(ctx->buf + idx, data, partial);
        md5_transform(ctx->s, ctx->buf);
        i = partial;
        while (i + 64 <= len) {
            md5_transform(ctx->s, data + i);
            i += 64;
        }
        idx = 0;
    }
    memcpy(ctx->buf + idx, data + i, len - i);
}

static void md5_final(md5_ctx_t *ctx, uint8_t digest[16]) {
    uint8_t bits[8];
    for (int i = 0; i < 4; i++) { bits[i] = (uint8_t)(ctx->count[0] >> (i*8)); bits[i+4] = (uint8_t)(ctx->count[1] >> (i*8)); }

    uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    pad = 0;
    while (((ctx->count[0] >> 3) & 63) != 56)
        md5_update(ctx, &pad, 1);
    md5_update(ctx, bits, 8);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            digest[i*4+j] = (uint8_t)(ctx->s[i] >> (j*8));
}

static void md5_hash(const uint8_t *data, size_t len, uint8_t out[16]) {
    md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, out);
}

/* RC4 cipher */
static void rc4_crypt(const uint8_t *key, int key_len, uint8_t *data, int data_len) {
    uint8_t S[256];
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % key_len]) & 255;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    int si = 0; j = 0;
    for (int k = 0; k < data_len; k++) {
        si = (si + 1) & 255;
        j = (j + S[si]) & 255;
        uint8_t t = S[si]; S[si] = S[j]; S[j] = t;
        data[k] ^= S[(S[si] + S[j]) & 255];
    }
}

/* AES-128-CBC decrypt (minimal implementation for PDF) */
/* AES S-box */
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

/* Inverse AES S-box */
static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

/* AES round constants */
static const uint8_t aes_rcon[11] = {0,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

/* GF(2^8) multiply */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        int hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* AES-128 key expansion */
static void aes128_key_expand(const uint8_t key[16], uint8_t rkeys[176]) {
    memcpy(rkeys, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t tmp[4];
        memcpy(tmp, rkeys + (i-1)*4, 4);
        if (i % 4 == 0) {
            uint8_t t = tmp[0];
            tmp[0] = aes_sbox[tmp[1]] ^ aes_rcon[i/4];
            tmp[1] = aes_sbox[tmp[2]];
            tmp[2] = aes_sbox[tmp[3]];
            tmp[3] = aes_sbox[t];
        }
        for (int j = 0; j < 4; j++)
            rkeys[i*4+j] = rkeys[(i-4)*4+j] ^ tmp[j];
    }
}

/* AES-128 decrypt a single 16-byte block */
static void aes128_decrypt_block(const uint8_t rkeys[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);

    /* Add round key 10 */
    for (int i = 0; i < 16; i++) s[i] ^= rkeys[160+i];

    for (int round = 9; round >= 1; round--) {
        /* Inv ShiftRows */
        uint8_t t;
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[10]; s[10] = s[2]; s[2] = t; t = s[14]; s[14] = s[6]; s[6] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        /* Inv SubBytes */
        for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
        /* Add round key */
        for (int i = 0; i < 16; i++) s[i] ^= rkeys[round*16+i];
        /* Inv MixColumns */
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = s[c*4], a1 = s[c*4+1], a2 = s[c*4+2], a3 = s[c*4+3];
            s[c*4]   = gf_mul(a0,0x0e) ^ gf_mul(a1,0x0b) ^ gf_mul(a2,0x0d) ^ gf_mul(a3,0x09);
            s[c*4+1] = gf_mul(a0,0x09) ^ gf_mul(a1,0x0e) ^ gf_mul(a2,0x0b) ^ gf_mul(a3,0x0d);
            s[c*4+2] = gf_mul(a0,0x0d) ^ gf_mul(a1,0x09) ^ gf_mul(a2,0x0e) ^ gf_mul(a3,0x0b);
            s[c*4+3] = gf_mul(a0,0x0b) ^ gf_mul(a1,0x0d) ^ gf_mul(a2,0x09) ^ gf_mul(a3,0x0e);
        }
    }

    /* Final round: Inv ShiftRows + Inv SubBytes + Add round key 0 */
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[10]; s[10] = s[2]; s[2] = t; t = s[14]; s[14] = s[6]; s[6] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
    for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];
    for (int i = 0; i < 16; i++) s[i] ^= rkeys[i];

    memcpy(out, s, 16);
}

/* AES-128-CBC decrypt */
static int aes128_cbc_decrypt(const uint8_t key[16], const uint8_t *data, int data_len,
                               uint8_t **out, int *out_len) {
    if (data_len < 16 || data_len % 16 != 0) return -1;

    /* First 16 bytes are IV */
    const uint8_t *iv = data;
    const uint8_t *ct = data + 16;
    int ct_len = data_len - 16;
    if (ct_len <= 0) return -1;

    uint8_t *pt = (uint8_t *)malloc(ct_len);
    if (!pt) return -1;

    uint8_t rkeys[176];
    aes128_key_expand(key, rkeys);

    const uint8_t *prev = iv;
    for (int i = 0; i < ct_len; i += 16) {
        uint8_t block[16];
        aes128_decrypt_block(rkeys, ct + i, block);
        for (int j = 0; j < 16; j++)
            pt[i + j] = block[j] ^ prev[j];
        prev = ct + i;
    }

    /* Remove PKCS#7 padding */
    int pad = pt[ct_len - 1];
    if (pad < 1 || pad > 16) pad = 0;
    int final_len = ct_len - pad;
    if (final_len < 0) final_len = 0;

    *out = pt;
    *out_len = final_len;
    return 0;
}

/* PDF password padding string */
static const uint8_t pdf_padding[32] = {
    0x28,0xBF,0x4E,0x5E,0x4D,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A
};

/* Compute encryption key from empty password */
static int pdf_compute_key(pdf_encrypt_t *enc) {
    md5_ctx_t md5;
    md5_init(&md5);

    /* Password (padded to 32 bytes with padding string) */
    md5_update(&md5, pdf_padding, 32);

    /* O entry */
    md5_update(&md5, enc->o, 32);

    /* P entry (little-endian 4 bytes) */
    uint8_t pbuf[4];
    pbuf[0] = (uint8_t)(enc->p); pbuf[1] = (uint8_t)(enc->p >> 8);
    pbuf[2] = (uint8_t)(enc->p >> 16); pbuf[3] = (uint8_t)(enc->p >> 24);
    md5_update(&md5, pbuf, 4);

    /* File ID */
    if (enc->has_file_id)
        md5_update(&md5, enc->file_id, 16);

    /* For R>=4, if metadata not encrypted, add 0xFFFFFFFF */

    uint8_t digest[16];
    md5_final(&md5, digest);

    /* For R>=3, do 50 more MD5 rounds */
    if (enc->r >= 3) {
        for (int i = 0; i < 50; i++)
            md5_hash(digest, enc->key_len, digest);
    }

    memcpy(enc->key, digest, enc->key_len);
    return 0;
}

/* Verify empty user password */
static int pdf_verify_password(pdf_encrypt_t *enc) {
    if (enc->r == 2) {
        /* R2: encrypt padding with key, compare to U */
        uint8_t test[32];
        memcpy(test, pdf_padding, 32);
        rc4_crypt(enc->key, enc->key_len, test, 32);
        return memcmp(test, enc->u, 32) == 0 ? 0 : -1;
    } else if (enc->r >= 3) {
        /* R3+: MD5(padding + fileID), then 20 rounds of RC4 with modified keys */
        md5_ctx_t md5;
        md5_init(&md5);
        md5_update(&md5, pdf_padding, 32);
        if (enc->has_file_id)
            md5_update(&md5, enc->file_id, 16);
        uint8_t digest[16];
        md5_final(&md5, digest);

        uint8_t test[16];
        memcpy(test, digest, 16);
        rc4_crypt(enc->key, enc->key_len, test, 16);

        for (int i = 1; i <= 19; i++) {
            uint8_t tkey[16];
            for (int j = 0; j < enc->key_len; j++)
                tkey[j] = enc->key[j] ^ (uint8_t)i;
            rc4_crypt(tkey, enc->key_len, test, 16);
        }

        return memcmp(test, enc->u, 16) == 0 ? 0 : -1;
    }
    return -1;
}

/* Setup encryption from trailer /Encrypt dict */
static int pdf_setup_encryption(pdf_doc_t *doc) {
    pdf_obj_t *encrypt_ref = dict_get(doc->trailer, "Encrypt");
    if (!encrypt_ref) return 0; /* no encryption */

    pdf_obj_t *encrypt = pdf_resolve(doc, encrypt_ref);
    if (!encrypt || encrypt->type != PDF_OBJ_DICT) return SW_PDF_ERR_ENCRYPT;

    pdf_encrypt_t *enc = &doc->encrypt;
    enc->enabled = 1;

    enc->v = (int)obj_int(dict_get(encrypt, "V"));
    enc->r = (int)obj_int(dict_get(encrypt, "R"));
    int length = (int)obj_int(dict_get(encrypt, "Length"));
    enc->key_len = (length > 0) ? length / 8 : 5;
    if (enc->key_len > 16) enc->key_len = 16;
    enc->p = (int32_t)obj_int(dict_get(encrypt, "P"));

    /* Determine method */
    enc->method = 0; /* RC4 default */
    if (enc->v == 4 || enc->r == 4) {
        pdf_obj_t *cf = dict_get(encrypt, "CF");
        if (cf) {
            pdf_obj_t *stdcf = dict_get(cf, "StdCF");
            if (stdcf) {
                stdcf = pdf_resolve(doc, stdcf);
                pdf_obj_t *cfm = dict_get(stdcf, "CFM");
                if (cfm && cfm->type == PDF_OBJ_NAME && strcmp(cfm->v.name, "AESV2") == 0)
                    enc->method = 1; /* AES-128 */
            }
        }
    }

    /* O and U entries */
    pdf_obj_t *o_obj = dict_get(encrypt, "O");
    pdf_obj_t *u_obj = dict_get(encrypt, "U");
    if (o_obj && o_obj->type == PDF_OBJ_STRING && o_obj->v.string.len >= 32)
        memcpy(enc->o, o_obj->v.string.data, 32);
    if (u_obj && u_obj->type == PDF_OBJ_STRING && u_obj->v.string.len >= 32)
        memcpy(enc->u, u_obj->v.string.data, 32);

    /* File ID */
    pdf_obj_t *id_arr = dict_get(doc->trailer, "ID");
    if (id_arr && id_arr->type == PDF_OBJ_ARRAY && id_arr->v.array.count > 0) {
        pdf_obj_t *id0 = id_arr->v.array.items[0];
        if (id0 && id0->type == PDF_OBJ_STRING && id0->v.string.len >= 16) {
            memcpy(enc->file_id, id0->v.string.data, 16);
            enc->has_file_id = 1;
        }
    }

    /* Compute key and verify */
    pdf_compute_key(enc);
    if (pdf_verify_password(enc) != 0) {
        /* Can't decrypt with empty password */
        return SW_PDF_ERR_ENCRYPT;
    }

    return SW_PDF_OK;
}

/* Per-object key derivation + decrypt */
static int pdf_decrypt_string(pdf_doc_t *doc, int objnum, int gen,
                               uint8_t *data, int len, uint8_t **out, int *out_len) {
    if (!doc->encrypt.enabled || len <= 0) {
        *out = NULL;
        *out_len = len;
        return 0;
    }

    pdf_encrypt_t *enc = &doc->encrypt;

    /* Derive per-object key: MD5(key + objnum(3 bytes LE) + gen(2 bytes LE) [+ "sAlT" for AES]) */
    uint8_t input[21 + 4]; /* max: 16 key + 3 + 2 + 4 */
    int input_len = enc->key_len;
    memcpy(input, enc->key, enc->key_len);
    input[input_len++] = (uint8_t)(objnum);
    input[input_len++] = (uint8_t)(objnum >> 8);
    input[input_len++] = (uint8_t)(objnum >> 16);
    input[input_len++] = (uint8_t)(gen);
    input[input_len++] = (uint8_t)(gen >> 8);

    if (enc->method == 1) {
        /* AES: append "sAlT" */
        input[input_len++] = 0x73; /* s */
        input[input_len++] = 0x41; /* A */
        input[input_len++] = 0x6C; /* l */
        input[input_len++] = 0x54; /* T */
    }

    uint8_t obj_key[16];
    md5_hash(input, input_len, obj_key);
    int obj_key_len = enc->key_len + 5;
    if (obj_key_len > 16) obj_key_len = 16;

    if (enc->method == 0) {
        /* RC4 */
        *out = (uint8_t *)malloc(len);
        if (!*out) return -1;
        memcpy(*out, data, len);
        rc4_crypt(obj_key, obj_key_len, *out, len);
        *out_len = len;
    } else {
        /* AES-128-CBC */
        if (aes128_cbc_decrypt(obj_key, data, len, out, out_len) != 0)
            return -1;
    }

    return 0;
}

/* =====================================================================
 * Section 6: Font Encoding / Unicode Mapper
 * ===================================================================== */

/* Parse a ToUnicode CMap stream */
static pdf_cmap_t *parse_tounicode_cmap(const uint8_t *data, size_t len) {
    pdf_cmap_t *cmap = (pdf_cmap_t *)calloc(1, sizeof(pdf_cmap_t));
    if (!cmap) return NULL;

    /* Detect if 2-byte CMap from codespacerange */
    const char *cs = "begincodespacerange";
    for (size_t i = 0; i + 20 < len; i++) {
        if (memcmp(data + i, cs, 19) == 0) {
            /* Look for hex range — if hex values are 4+ hex chars → 2-byte */
            for (size_t j = i + 19; j + 4 < len; j++) {
                if (data[j] == '<') {
                    int hex_chars = 0;
                    for (size_t k = j + 1; k < len && data[k] != '>'; k++) hex_chars++;
                    if (hex_chars >= 4) cmap->is_two_byte = 1;
                    break;
                }
                if (memcmp(data + j, "endcodespacerange", 17) == 0) break;
            }
            break;
        }
    }

    /* Parse beginbfchar sections */
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    while (p < end) {
        /* Find beginbfchar */
        if (p + 12 < end && memcmp(p, "beginbfchar", 11) == 0) {
            p += 11;
            while (p < end) {
                /* skip ws */
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (p + 9 < end && memcmp(p, "endbfchar", 9) == 0) { p += 9; break; }

                /* Parse: <srcCode> <dstUnicode> */
                if (*p != '<') { p++; continue; }
                p++; /* skip < */
                uint32_t src = 0;
                int src_digits = 0;
                while (p < end && *p != '>') {
                    int d = hex_digit(*p);
                    if (d >= 0) { src = (src << 4) | d; src_digits++; }
                    p++;
                }
                if (p < end) p++; /* skip > */

                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;

                if (p < end && *p == '<') {
                    p++; /* skip < */
                    uint32_t dst = 0;
                    while (p < end && *p != '>') {
                        int d = hex_digit(*p);
                        if (d >= 0) dst = (dst << 4) | d;
                        p++;
                    }
                    if (p < end) p++; /* skip > */

                    if (src < PDF_CMAP_MAX) {
                        cmap->map[src] = dst;
                        if ((int)src > cmap->max_code) cmap->max_code = (int)src;
                    }
                    (void)src_digits;
                }
            }
            continue;
        }

        /* Find beginbfrange */
        if (p + 13 < end && memcmp(p, "beginbfrange", 12) == 0) {
            p += 12;
            while (p < end) {
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (p + 10 < end && memcmp(p, "endbfrange", 10) == 0) { p += 10; break; }

                /* Parse: <srcLo> <srcHi> <dstLo> or <srcLo> <srcHi> [<d1> <d2> ...] */
                uint32_t src_lo = 0, src_hi = 0;

                if (p >= end || *p != '<') { p++; continue; }
                p++;
                while (p < end && *p != '>') { int d = hex_digit(*p); if (d >= 0) src_lo = (src_lo << 4) | d; p++; }
                if (p < end) p++;

                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
                if (p >= end || *p != '<') { p++; continue; }
                p++;
                while (p < end && *p != '>') { int d = hex_digit(*p); if (d >= 0) src_hi = (src_hi << 4) | d; p++; }
                if (p < end) p++;

                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;

                if (p < end && *p == '[') {
                    /* Array of destination values */
                    p++;
                    for (uint32_t code = src_lo; code <= src_hi && code < PDF_CMAP_MAX; code++) {
                        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
                        if (p >= end || *p == ']') break;
                        if (*p == '<') {
                            p++;
                            uint32_t dst = 0;
                            while (p < end && *p != '>') { int d = hex_digit(*p); if (d >= 0) dst = (dst << 4) | d; p++; }
                            if (p < end) p++;
                            cmap->map[code] = dst;
                            if ((int)code > cmap->max_code) cmap->max_code = (int)code;
                        }
                    }
                    while (p < end && *p != ']') p++;
                    if (p < end) p++;
                } else if (p < end && *p == '<') {
                    /* Single destination base */
                    p++;
                    uint32_t dst = 0;
                    while (p < end && *p != '>') { int d = hex_digit(*p); if (d >= 0) dst = (dst << 4) | d; p++; }
                    if (p < end) p++;

                    for (uint32_t code = src_lo; code <= src_hi && code < PDF_CMAP_MAX; code++) {
                        cmap->map[code] = dst + (code - src_lo);
                        if ((int)code > cmap->max_code) cmap->max_code = (int)code;
                    }
                }
            }
            continue;
        }

        p++;
    }

    return cmap;
}

/* Resolve font encoding from a font dictionary */
static pdf_font_t *resolve_font(pdf_doc_t *doc, pdf_obj_t *font_dict) {
    if (!font_dict) return NULL;
    font_dict = pdf_resolve(doc, font_dict);
    if (!font_dict || font_dict->type != PDF_OBJ_DICT) return NULL;

    pdf_font_t *font = (pdf_font_t *)calloc(1, sizeof(pdf_font_t));
    if (!font) return NULL;

    /* Default encoding */
    font->encoding = winansi_to_unicode;

    /* Font name */
    pdf_obj_t *name = dict_get(font_dict, "BaseFont");
    if (name) {
        const char *n = obj_name(name);
        if (n) { strncpy(font->name, n, sizeof(font->name) - 1); }
    }

    /* Check for CIDFont (Type0) */
    pdf_obj_t *subtype = dict_get(font_dict, "Subtype");
    if (subtype && obj_name(subtype) && strcmp(obj_name(subtype), "Type0") == 0) {
        font->is_cid = 1;
    }

    /* ToUnicode CMap (highest priority for mapping) */
    pdf_obj_t *tounicode = dict_get(font_dict, "ToUnicode");
    if (tounicode) {
        tounicode = pdf_resolve(doc, tounicode);
        if (tounicode && tounicode->type == PDF_OBJ_STREAM) {
            uint8_t *cmap_data = NULL;
            size_t cmap_len = 0;
            if (pdf_decompress_stream(doc, tounicode,
                    tounicode->v.stream.objnum, tounicode->v.stream.gen,
                    &cmap_data, &cmap_len) == 0) {
                font->tounicode = parse_tounicode_cmap(cmap_data, cmap_len);
                free(cmap_data);
            }
        }
    }

    /* Encoding */
    pdf_obj_t *enc_obj = dict_get(font_dict, "Encoding");
    if (enc_obj) {
        enc_obj = pdf_resolve(doc, enc_obj);
        if (enc_obj && enc_obj->type == PDF_OBJ_NAME) {
            const char *enc_name = enc_obj->v.name;
            if (strcmp(enc_name, "WinAnsiEncoding") == 0)
                font->encoding = winansi_to_unicode;
            else if (strcmp(enc_name, "MacRomanEncoding") == 0)
                font->encoding = macroman_to_unicode;
            else if (strcmp(enc_name, "StandardEncoding") == 0)
                font->encoding = standard_to_unicode;
        } else if (enc_obj && enc_obj->type == PDF_OBJ_DICT) {
            /* Encoding dict with /BaseEncoding and /Differences */
            pdf_obj_t *base = dict_get(enc_obj, "BaseEncoding");
            if (base && base->type == PDF_OBJ_NAME) {
                if (strcmp(base->v.name, "WinAnsiEncoding") == 0)
                    font->encoding = winansi_to_unicode;
                else if (strcmp(base->v.name, "MacRomanEncoding") == 0)
                    font->encoding = macroman_to_unicode;
                else if (strcmp(base->v.name, "StandardEncoding") == 0)
                    font->encoding = standard_to_unicode;
            }

            /* /Differences array: [code /name /name code /name ...] */
            pdf_obj_t *diff = dict_get(enc_obj, "Differences");
            if (diff && diff->type == PDF_OBJ_ARRAY) {
                int cur_code = 0;
                for (int i = 0; i < diff->v.array.count; i++) {
                    pdf_obj_t *item = diff->v.array.items[i];
                    if (item->type == PDF_OBJ_INT) {
                        cur_code = (int)item->v.integer;
                    } else if (item->type == PDF_OBJ_NAME && cur_code < 256) {
                        /* Look up glyph name in AGL */
                        uint16_t u = agl_lookup(item->v.name);
                        if (u == 0 && strlen(item->v.name) > 3 && item->v.name[0] == 'u' && item->v.name[1] == 'n' && item->v.name[2] == 'i') {
                            /* uniXXXX format */
                            u = (uint16_t)strtol(item->v.name + 3, NULL, 16);
                        }
                        if (u > 0) {
                            font->differences[cur_code] = u;
                            font->has_differences = 1;
                        }
                        cur_code++;
                    }
                }
            }
        }
    }

    /* For descendant fonts (CIDFont) — check DescendantFonts */
    if (font->is_cid) {
        pdf_obj_t *desc = dict_get(font_dict, "DescendantFonts");
        if (desc) {
            desc = pdf_resolve(doc, desc);
            if (desc && desc->type == PDF_OBJ_ARRAY && desc->v.array.count > 0) {
                pdf_obj_t *cid_font = pdf_resolve(doc, desc->v.array.items[0]);
                if (cid_font && cid_font->type == PDF_OBJ_DICT) {
                    /* Could extract CIDToGIDMap, but ToUnicode CMap is sufficient for text */
                    (void)cid_font;
                }
            }
        }
    }

    return font;
}

/* Map a character code to Unicode using font encoding */
static uint32_t font_decode_char(pdf_font_t *font, uint32_t code) {
    if (!font) return code; /* no font info — pass through */

    /* Priority 1: ToUnicode CMap */
    if (font->tounicode && code <= (uint32_t)font->tounicode->max_code) {
        uint32_t u = font->tounicode->map[code];
        if (u > 0) return u;
    }

    /* Priority 2: Differences array */
    if (font->has_differences && code < 256 && font->differences[code] > 0)
        return font->differences[code];

    /* Priority 3: Base encoding table */
    if (code < 256 && font->encoding) {
        uint16_t u = font->encoding[code];
        if (u > 0) return u;
    }

    /* Priority 4: Latin-1 fallback */
    return code;
}

/* Free a font */
static void font_free(pdf_font_t *font) {
    if (!font) return;
    free(font->tounicode);
    free(font);
}

/* Font cache for a page extraction */
#define MAX_FONTS_PER_PAGE 64
typedef struct {
    char name[64];
    pdf_font_t *font;
} font_cache_entry_t;

typedef struct {
    font_cache_entry_t entries[MAX_FONTS_PER_PAGE];
    int count;
} font_cache_t;

static pdf_font_t *font_cache_get(font_cache_t *cache, const char *name) {
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i].name, name) == 0)
            return cache->entries[i].font;
    }
    return NULL;
}

static void font_cache_put(font_cache_t *cache, const char *name, pdf_font_t *font) {
    if (cache->count >= MAX_FONTS_PER_PAGE) return;
    strncpy(cache->entries[cache->count].name, name, 63);
    cache->entries[cache->count].font = font;
    cache->count++;
}

static void font_cache_free(font_cache_t *cache) {
    for (int i = 0; i < cache->count; i++)
        font_free(cache->entries[i].font);
    cache->count = 0;
}

/* =====================================================================
 * Section 7: Content Stream Interpreter (Text State Machine)
 * ===================================================================== */

/* Initialize text extraction context */
static void text_ctx_init(pdf_text_ctx_t *ctx) {
    memset(ctx, 0, sizeof(pdf_text_ctx_t));
    ctx->cap = PDF_INITIAL_BUF;
    ctx->buf = (char *)malloc(ctx->cap);
    if (ctx->buf) ctx->buf[0] = '\0';
    mat_identity(ctx->gs.ctm);
    mat_identity(ctx->gs.tm);
    mat_identity(ctx->gs.tlm);
    ctx->gs.th = 1.0f; /* 100% horizontal scaling */
    ctx->gs.font_size = 12.0f;
    ctx->last_x = -999999.0f;
    ctx->last_y = -999999.0f;
}

/* Emit text from a string object using current font encoding */
static void emit_text_string(pdf_text_ctx_t *ctx, const uint8_t *str, int slen, pdf_font_t *font) {
    if (!ctx->buf || slen <= 0) return;

    /* Calculate current text position */
    float x = ctx->gs.tm[4];
    float y = ctx->gs.tm[5];

    /* Detect position changes for whitespace heuristic */
    if (ctx->had_text) {
        float dy = fabsf(y - ctx->last_y);
        float dx = x - ctx->last_x;
        float fs = ctx->gs.font_size;
        if (fs < 1.0f) fs = 12.0f;

        /* Vertical gap → newline */
        if (dy > fs * 0.5f) {
            buf_append_char(ctx, '\n');
        } else if (dx > fs * 0.3f) {
            /* Horizontal gap → space */
            buf_append_char(ctx, ' ');
        }
    }

    /* Decode and emit characters */
    if (font && font->is_cid && font->tounicode) {
        /* 2-byte CID font */
        for (int i = 0; i + 1 < slen; i += 2) {
            uint32_t code = ((uint32_t)str[i] << 8) | str[i + 1];
            uint32_t u = font_decode_char(font, code);
            if (u > 0 && u != 0xFFFF) buf_append_unicode(ctx, u);
        }
    } else {
        /* Single-byte font */
        for (int i = 0; i < slen; i++) {
            uint32_t code = str[i];
            uint32_t u = font_decode_char(font, code);
            if (u > 0 && u != 0xFFFF) buf_append_unicode(ctx, u);
        }
    }

    /* Update position tracking */
    /* Advance text position by approximate string width */
    float advance = 0;
    if (font && font->is_cid) {
        advance = (slen / 2) * ctx->gs.font_size * ctx->gs.th * 0.5f;
    } else {
        advance = slen * ctx->gs.font_size * ctx->gs.th * 0.5f;
    }
    ctx->gs.tm[4] += advance;
    ctx->last_x = ctx->gs.tm[4];
    ctx->last_y = y;
    ctx->had_text = 1;
}

/* Parse a content stream operand stack value */
typedef struct {
    enum { OP_NUM, OP_STR, OP_NAME, OP_ARRAY } type;
    union {
        double num;
        struct { uint8_t *data; int len; } str;  /* heap-allocated, owned */
        const char *name;
        struct { pdf_obj_t *obj; } array;
    } v;
} cs_operand_t;

/* Free operand resources */
static void cs_ops_free(cs_operand_t *ops, int nops) {
    for (int i = 0; i < nops; i++) {
        if (ops[i].type == OP_STR) free(ops[i].v.str.data);
        else if (ops[i].type == OP_ARRAY) pdf_obj_free(ops[i].v.array.obj);
    }
}

#define CS_MAX_OPERANDS 64

/* Forward declaration */
static void extract_from_content_stream(pdf_doc_t *doc, pdf_text_ctx_t *ctx,
                                         const uint8_t *data, size_t len,
                                         pdf_obj_t *resources, font_cache_t *fc, int depth);

/* Process a single content stream with operand tracking */
static void extract_from_content_stream(pdf_doc_t *doc, pdf_text_ctx_t *ctx,
                                         const uint8_t *data, size_t len,
                                         pdf_obj_t *resources, font_cache_t *fc, int depth) {
    if (depth > 10 || !data || len == 0) return;

    cs_operand_t ops[CS_MAX_OPERANDS];
    int nops = 0;
    int in_text = 0; /* inside BT..ET */
    (void)in_text;
    size_t pos = 0;

    /* Resolve resources */
    resources = pdf_resolve(doc, resources);
    pdf_obj_t *fonts_dict = resources ? pdf_resolve(doc, dict_get(resources, "Font")) : NULL;
    pdf_obj_t *xobjects = resources ? pdf_resolve(doc, dict_get(resources, "XObject")) : NULL;

    while (pos < len) {
        /* Skip whitespace */
        while (pos < len && is_pdf_ws(data[pos])) pos++;
        if (pos >= len) break;

        uint8_t c = data[pos];

        /* Comment */
        if (c == '%') {
            while (pos < len && data[pos] != '\n' && data[pos] != '\r') pos++;
            continue;
        }

        /* Literal string (...) */
        if (c == '(') {
            pos++;
            /* Parse with escape handling */
            int sbuf_cap = 8192;
            uint8_t *sbuf = (uint8_t *)malloc(sbuf_cap);
            if (!sbuf) break;
            int slen = 0;
            int sdepth = 1;
            while (pos < len && sdepth > 0) {
                if (slen >= sbuf_cap - 4) {
                    int nc = sbuf_cap * 2;
                    uint8_t *nb = (uint8_t *)realloc(sbuf, nc);
                    if (!nb) break;
                    sbuf = nb; sbuf_cap = nc;
                }
                c = data[pos];
                if (c == '(') { sdepth++; sbuf[slen++] = c; pos++; }
                else if (c == ')') { sdepth--; if (sdepth > 0) sbuf[slen++] = c; pos++; }
                else if (c == '\\') {
                    pos++;
                    if (pos >= len) break;
                    c = data[pos++];
                    switch (c) {
                        case 'n': sbuf[slen++] = '\n'; break;
                        case 'r': sbuf[slen++] = '\r'; break;
                        case 't': sbuf[slen++] = '\t'; break;
                        case 'b': sbuf[slen++] = '\b'; break;
                        case 'f': sbuf[slen++] = '\f'; break;
                        case '(': sbuf[slen++] = '('; break;
                        case ')': sbuf[slen++] = ')'; break;
                        case '\\': sbuf[slen++] = '\\'; break;
                        case '\r':
                            if (pos < len && data[pos] == '\n') pos++;
                            break;
                        case '\n': break;
                        default:
                            if (c >= '0' && c <= '7') {
                                int oct = c - '0';
                                if (pos < len && data[pos] >= '0' && data[pos] <= '7')
                                    oct = oct * 8 + (data[pos++] - '0');
                                if (pos < len && data[pos] >= '0' && data[pos] <= '7')
                                    oct = oct * 8 + (data[pos++] - '0');
                                sbuf[slen++] = (uint8_t)(oct & 0xFF);
                            } else {
                                sbuf[slen++] = c;
                            }
                            break;
                    }
                } else {
                    sbuf[slen++] = c;
                    pos++;
                }
            }
            /* Skip remaining if string was too long */
            while (pos < len && sdepth > 0) {
                if (data[pos] == '(') sdepth++;
                else if (data[pos] == ')') sdepth--;
                pos++;
            }

            if (nops < CS_MAX_OPERANDS) {
                ops[nops].type = OP_STR;
                ops[nops].v.str.data = sbuf;
                ops[nops].v.str.len = slen;
                nops++;
            } else {
                free(sbuf);
            }
            continue;
        }

        /* Hex string <...> */
        if (c == '<' && pos + 1 < len && data[pos + 1] != '<') {
            pos++;
            int hbuf_cap = 4096;
            uint8_t *hbuf = (uint8_t *)malloc(hbuf_cap);
            if (!hbuf) break;
            int hlen = 0;
            int high = -1;
            while (pos < len && data[pos] != '>') {
                if (hlen >= hbuf_cap - 1) {
                    int nc = hbuf_cap * 2;
                    uint8_t *nb = (uint8_t *)realloc(hbuf, nc);
                    if (!nb) break;
                    hbuf = nb; hbuf_cap = nc;
                }
                int d = hex_digit(data[pos]);
                if (d >= 0) {
                    if (high < 0) high = d;
                    else { hbuf[hlen++] = (uint8_t)((high << 4) | d); high = -1; }
                }
                pos++;
            }
            if (high >= 0) hbuf[hlen++] = (uint8_t)(high << 4);
            if (pos < len) pos++; /* skip > */

            if (nops < CS_MAX_OPERANDS) {
                ops[nops].type = OP_STR;
                ops[nops].v.str.data = hbuf;
                ops[nops].v.str.len = hlen;
                nops++;
            } else {
                free(hbuf);
            }
            continue;
        }

        /* Array [...] — used by TJ */
        if (c == '[') {
            /* Parse TJ array inline: mix of strings and numbers */
            pos++;
            /* We handle TJ arrays specially at operator dispatch time */
            /* For now, parse into a temporary object */
            pdf_lexer_t alex = { data, len, pos - 1 }; /* back up to [ */
            pdf_obj_t *arr = lex_parse_array(&alex);
            pos = alex.pos;
            if (arr && nops < CS_MAX_OPERANDS) {
                ops[nops].type = OP_ARRAY;
                ops[nops].v.array.obj = arr;
                nops++;
            } else {
                pdf_obj_free(arr);
            }
            continue;
        }

        /* Dict << >> — skip (inline images etc.) */
        if (c == '<' && pos + 1 < len && data[pos + 1] == '<') {
            /* Skip dict — not relevant for text extraction */
            int dd = 1;
            pos += 2;
            while (pos + 1 < len && dd > 0) {
                if (data[pos] == '<' && data[pos + 1] == '<') { dd++; pos += 2; }
                else if (data[pos] == '>' && data[pos + 1] == '>') { dd--; pos += 2; }
                else pos++;
            }
            continue;
        }

        /* Name /Something */
        if (c == '/') {
            pos++;
            char nbuf[128];
            int nlen = 0;
            while (pos < len && nlen < 126 && !is_pdf_ws(data[pos]) && !is_pdf_delim(data[pos])) {
                if (data[pos] == '#' && pos + 2 < len) {
                    int h1 = hex_digit(data[pos+1]), h2 = hex_digit(data[pos+2]);
                    if (h1 >= 0 && h2 >= 0) { nbuf[nlen++] = (char)((h1<<4)|h2); pos += 3; continue; }
                }
                nbuf[nlen++] = (char)data[pos++];
            }
            nbuf[nlen] = '\0';

            if (nops < CS_MAX_OPERANDS) {
                ops[nops].type = OP_NAME;
                ops[nops].v.name = nbuf; /* stack-local, consumed before next iteration */
                nops++;
            }
            continue;
        }

        /* Number */
        if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            size_t start = pos;
            if (data[pos] == '+' || data[pos] == '-') pos++;
            while (pos < len && ((data[pos] >= '0' && data[pos] <= '9') || data[pos] == '.')) pos++;
            char tmp[64];
            size_t tl = pos - start;
            if (tl >= sizeof(tmp)) tl = sizeof(tmp) - 1;
            memcpy(tmp, data + start, tl);
            tmp[tl] = '\0';

            if (nops < CS_MAX_OPERANDS) {
                ops[nops].type = OP_NUM;
                ops[nops].v.num = atof(tmp);
                nops++;
            }
            continue;
        }

        /* Operator keyword */
        if (isalpha(c) || c == '\'' || c == '"' || c == '*') {
            char op[16];
            int olen = 0;
            while (pos < len && olen < 14 && !is_pdf_ws(data[pos]) && !is_pdf_delim(data[pos])) {
                op[olen++] = (char)data[pos++];
            }
            op[olen] = '\0';

            /* Dispatch text operators */
            if (strcmp(op, "BT") == 0) {
                in_text = 1;
                mat_identity(ctx->gs.tm);
                mat_identity(ctx->gs.tlm);
            } else if (strcmp(op, "ET") == 0) {
                in_text = 0;
            } else if (strcmp(op, "Tf") == 0 && nops >= 2) {
                /* Set font: /FontName size Tf */
                const char *fname = NULL;
                double fsize = 0;
                /* Find name and number operands */
                for (int i = 0; i < nops; i++) {
                    if (ops[i].type == OP_NAME) fname = ops[i].v.name;
                    if (ops[i].type == OP_NUM) fsize = ops[i].v.num;
                }
                if (fname && fonts_dict) {
                    /* Look up font in resources */
                    pdf_font_t *f = font_cache_get(fc, fname);
                    if (!f) {
                        pdf_obj_t *fobj = dict_get(fonts_dict, fname);
                        if (fobj) {
                            f = resolve_font(doc, fobj);
                            if (f) font_cache_put(fc, fname, f);
                        }
                    }
                    ctx->gs.font = f;
                }
                if (fsize != 0) ctx->gs.font_size = (float)fsize;
            } else if (strcmp(op, "Td") == 0 && nops >= 2) {
                /* Move text position: tx ty Td */
                float tx = (float)ops[nops-2].v.num;
                float ty = (float)ops[nops-1].v.num;
                ctx->gs.tlm[4] += tx * ctx->gs.tlm[0] + ty * ctx->gs.tlm[2];
                ctx->gs.tlm[5] += tx * ctx->gs.tlm[1] + ty * ctx->gs.tlm[3];
                memcpy(ctx->gs.tm, ctx->gs.tlm, sizeof(float) * 6);
            } else if (strcmp(op, "TD") == 0 && nops >= 2) {
                /* Move text position + set leading: tx ty TD = -ty TL; tx ty Td */
                float tx = (float)ops[nops-2].v.num;
                float ty = (float)ops[nops-1].v.num;
                ctx->gs.tl = -ty;
                ctx->gs.tlm[4] += tx * ctx->gs.tlm[0] + ty * ctx->gs.tlm[2];
                ctx->gs.tlm[5] += tx * ctx->gs.tlm[1] + ty * ctx->gs.tlm[3];
                memcpy(ctx->gs.tm, ctx->gs.tlm, sizeof(float) * 6);
            } else if (strcmp(op, "Tm") == 0 && nops >= 6) {
                /* Set text matrix: a b c d e f Tm */
                ctx->gs.tm[0] = (float)ops[nops-6].v.num;
                ctx->gs.tm[1] = (float)ops[nops-5].v.num;
                ctx->gs.tm[2] = (float)ops[nops-4].v.num;
                ctx->gs.tm[3] = (float)ops[nops-3].v.num;
                ctx->gs.tm[4] = (float)ops[nops-2].v.num;
                ctx->gs.tm[5] = (float)ops[nops-1].v.num;
                memcpy(ctx->gs.tlm, ctx->gs.tm, sizeof(float) * 6);
            } else if (strcmp(op, "T*") == 0) {
                /* Move to start of next line: 0 -TL Td */
                float ty = -ctx->gs.tl;
                ctx->gs.tlm[4] += ty * ctx->gs.tlm[2];
                ctx->gs.tlm[5] += ty * ctx->gs.tlm[3];
                memcpy(ctx->gs.tm, ctx->gs.tlm, sizeof(float) * 6);
            } else if (strcmp(op, "Tc") == 0 && nops >= 1) {
                ctx->gs.tc = (float)ops[nops-1].v.num;
            } else if (strcmp(op, "Tw") == 0 && nops >= 1) {
                ctx->gs.tw = (float)ops[nops-1].v.num;
            } else if (strcmp(op, "Tz") == 0 && nops >= 1) {
                ctx->gs.th = (float)(ops[nops-1].v.num / 100.0);
            } else if (strcmp(op, "TL") == 0 && nops >= 1) {
                ctx->gs.tl = (float)ops[nops-1].v.num;
            } else if (strcmp(op, "Ts") == 0 && nops >= 1) {
                ctx->gs.trise = (float)ops[nops-1].v.num;
            } else if (strcmp(op, "Tj") == 0 && nops >= 1) {
                /* Show string: (string) Tj */
                for (int i = nops - 1; i >= 0; i--) {
                    if (ops[i].type == OP_STR) {
                        emit_text_string(ctx, ops[i].v.str.data, ops[i].v.str.len, ctx->gs.font);
                        break;
                    }
                }
            } else if (strcmp(op, "TJ") == 0 && nops >= 1) {
                /* Show strings with individual positioning: [...] TJ */
                for (int i = nops - 1; i >= 0; i--) {
                    if (ops[i].type == OP_ARRAY && ops[i].v.array.obj) {
                        pdf_obj_t *arr = ops[i].v.array.obj;
                        for (int j = 0; j < arr->v.array.count; j++) {
                            pdf_obj_t *elem = arr->v.array.items[j];
                            if (elem->type == PDF_OBJ_STRING) {
                                emit_text_string(ctx, (const uint8_t *)elem->v.string.data,
                                                 elem->v.string.len, ctx->gs.font);
                            } else if (elem->type == PDF_OBJ_INT || elem->type == PDF_OBJ_REAL) {
                                /* Negative values = move right (add space if large enough) */
                                double adj = obj_real(elem);
                                if (adj < -100) {
                                    buf_append_char(ctx, ' ');
                                }
                                /* Adjust text position */
                                float dx = (float)(-adj / 1000.0 * ctx->gs.font_size * ctx->gs.th);
                                ctx->gs.tm[4] += dx;
                            }
                        }
                        break;
                    }
                }
            } else if (strcmp(op, "'") == 0 && nops >= 1) {
                /* Move to next line and show string: T* then Tj */
                float ty = -ctx->gs.tl;
                ctx->gs.tlm[4] += ty * ctx->gs.tlm[2];
                ctx->gs.tlm[5] += ty * ctx->gs.tlm[3];
                memcpy(ctx->gs.tm, ctx->gs.tlm, sizeof(float) * 6);
                for (int i = nops - 1; i >= 0; i--) {
                    if (ops[i].type == OP_STR) {
                        emit_text_string(ctx, ops[i].v.str.data, ops[i].v.str.len, ctx->gs.font);
                        break;
                    }
                }
            } else if (strcmp(op, "\"") == 0 && nops >= 3) {
                /* Set spacing, move to next line, show string: aw ac string " */
                ctx->gs.tw = (float)ops[nops-3].v.num;
                ctx->gs.tc = (float)ops[nops-2].v.num;
                float ty = -ctx->gs.tl;
                ctx->gs.tlm[4] += ty * ctx->gs.tlm[2];
                ctx->gs.tlm[5] += ty * ctx->gs.tlm[3];
                memcpy(ctx->gs.tm, ctx->gs.tlm, sizeof(float) * 6);
                if (ops[nops-1].type == OP_STR) {
                    emit_text_string(ctx, ops[nops-1].v.str.data, ops[nops-1].v.str.len, ctx->gs.font);
                }
            } else if (strcmp(op, "cm") == 0 && nops >= 6) {
                /* Concat matrix */
                float m[6];
                m[0] = (float)ops[nops-6].v.num; m[1] = (float)ops[nops-5].v.num;
                m[2] = (float)ops[nops-4].v.num; m[3] = (float)ops[nops-3].v.num;
                m[4] = (float)ops[nops-2].v.num; m[5] = (float)ops[nops-1].v.num;
                float r[6];
                mat_multiply(r, m, ctx->gs.ctm);
                memcpy(ctx->gs.ctm, r, sizeof(float) * 6);
            } else if (strcmp(op, "q") == 0) {
                /* Push graphics state */
                if (ctx->gs_top < PDF_MAX_GSTATE_STACK - 1) {
                    ctx->gs_stack[ctx->gs_top++] = ctx->gs;
                }
            } else if (strcmp(op, "Q") == 0) {
                /* Pop graphics state */
                if (ctx->gs_top > 0) {
                    pdf_font_t *cur_font = ctx->gs.font;
                    ctx->gs = ctx->gs_stack[--ctx->gs_top];
                    /* Keep font reference (managed by cache, not by gstate) */
                    (void)cur_font;
                }
            } else if (strcmp(op, "Do") == 0 && nops >= 1) {
                /* Invoke XObject — recurse if it's a Form */
                const char *xname = NULL;
                for (int i = nops - 1; i >= 0; i--) {
                    if (ops[i].type == OP_NAME) { xname = ops[i].v.name; break; }
                }
                if (xname && xobjects) {
                    pdf_obj_t *xobj = pdf_resolve(doc, dict_get(xobjects, xname));
                    if (xobj && xobj->type == PDF_OBJ_STREAM) {
                        pdf_obj_t *xdict = xobj->v.stream.dict_obj;
                        pdf_obj_t *xsubtype = dict_get(xdict, "Subtype");
                        if (xsubtype && obj_name(xsubtype) && strcmp(obj_name(xsubtype), "Form") == 0) {
                            uint8_t *xdata = NULL;
                            size_t xlen = 0;
                            if (pdf_decompress_stream(doc, xobj,
                                    xobj->v.stream.objnum, xobj->v.stream.gen,
                                    &xdata, &xlen) == 0) {
                                pdf_obj_t *xres = dict_get(xdict, "Resources");
                                if (!xres) xres = resources;
                                extract_from_content_stream(doc, ctx, xdata, xlen,
                                                            xres, fc, depth + 1);
                                free(xdata);
                            }
                        }
                    }
                }
            } else if (strcmp(op, "BI") == 0) {
                /* Begin inline image — skip until EI */
                while (pos + 2 < len) {
                    if (data[pos] == 'E' && data[pos + 1] == 'I' &&
                        (pos + 2 >= len || is_pdf_ws(data[pos + 2]))) {
                        pos += 2;
                        break;
                    }
                    pos++;
                }
            }

            /* Free operand resources */
            cs_ops_free(ops, nops);
            nops = 0;
            continue;
        }

        /* Unknown byte — skip */
        pos++;
    }

    /* Clean up any remaining operands */
    cs_ops_free(ops, nops);
}

/* =====================================================================
 * Section 8: Page Tree Walker + Public API
 * ===================================================================== */

/* Collect page objects from the page tree into a flat array.
 * page_resources[i] stores the inherited resources for pages[i] when the
 * page dict itself has no /Resources entry. */
static void collect_pages(pdf_doc_t *doc, pdf_obj_t *node,
                          pdf_obj_t **pages, pdf_obj_t **page_resources,
                          int *count, int max,
                          pdf_obj_t *inherited_resources) {
    if (!node || *count >= max) return;
    node = pdf_resolve(doc, node);
    if (!node || node->type != PDF_OBJ_DICT) return;

    pdf_obj_t *type = dict_get(node, "Type");
    const char *tname = obj_name(type);

    /* Inherit resources from parent if not overridden */
    pdf_obj_t *res = dict_get(node, "Resources");
    if (!res) res = inherited_resources;

    if (tname && strcmp(tname, "Pages") == 0) {
        /* Pages node — recurse into Kids */
        pdf_obj_t *kids = pdf_resolve(doc, dict_get(node, "Kids"));
        if (kids && kids->type == PDF_OBJ_ARRAY) {
            for (int i = 0; i < kids->v.array.count && *count < max; i++) {
                collect_pages(doc, kids->v.array.items[i], pages, page_resources,
                              count, max, res);
            }
        }
    } else if (tname && strcmp(tname, "Page") == 0) {
        pages[*count] = node;
        page_resources[*count] = res; /* may be NULL or inherited */
        (*count)++;
    }
}

/* Extract text from a single page. inherited_res is used when the page has no /Resources. */
static int extract_page_text(pdf_doc_t *doc, pdf_obj_t *page, pdf_text_ctx_t *ctx,
                              pdf_obj_t *inherited_res) {
    if (!page || page->type != PDF_OBJ_DICT) return SW_PDF_ERR_PARSE;

    pdf_obj_t *resources = pdf_resolve(doc, dict_get(page, "Resources"));
    if (!resources) resources = pdf_resolve(doc, inherited_res);
    pdf_obj_t *contents = dict_get(page, "Contents");
    if (!contents) return SW_PDF_OK; /* empty page */

    contents = pdf_resolve(doc, contents);
    if (!contents) return SW_PDF_OK;

    font_cache_t fc;
    memset(&fc, 0, sizeof(fc));

    if (contents->type == PDF_OBJ_STREAM) {
        /* Single content stream */
        uint8_t *cdata = NULL;
        size_t clen = 0;
        if (pdf_decompress_stream(doc, contents,
                contents->v.stream.objnum, contents->v.stream.gen,
                &cdata, &clen) == 0) {
            extract_from_content_stream(doc, ctx, cdata, clen, resources, &fc, 0);
            free(cdata);
        }
    } else if (contents->type == PDF_OBJ_ARRAY) {
        /* Multiple content streams — concatenate */
        for (int i = 0; i < contents->v.array.count; i++) {
            pdf_obj_t *stm = pdf_resolve(doc, contents->v.array.items[i]);
            if (stm && stm->type == PDF_OBJ_STREAM) {
                uint8_t *cdata = NULL;
                size_t clen = 0;
                if (pdf_decompress_stream(doc, stm,
                        stm->v.stream.objnum, stm->v.stream.gen,
                        &cdata, &clen) == 0) {
                    extract_from_content_stream(doc, ctx, cdata, clen, resources, &fc, 0);
                    free(cdata);
                }
            }
        }
    }

    font_cache_free(&fc);
    return SW_PDF_OK;
}

/* Open and parse a PDF document from memory */
static int pdf_doc_open(pdf_doc_t *doc, const uint8_t *data, size_t len) {
    memset(doc, 0, sizeof(pdf_doc_t));
    doc->data = data;
    doc->len = len;

    /* Verify PDF header */
    if (len < 8 || memcmp(data, "%PDF-", 5) != 0)
        return SW_PDF_ERR_PARSE;

    /* Load cross-reference table */
    int rc = load_xref(doc);
    if (rc != SW_PDF_OK) return rc;

    /* Allocate object cache */
    doc->obj_cache = (pdf_obj_t **)calloc(doc->xref_count, sizeof(pdf_obj_t *));

    /* Setup encryption (if any) */
    rc = pdf_setup_encryption(doc);
    if (rc != SW_PDF_OK) return rc;

    return SW_PDF_OK;
}

/* Close document and free resources */
static void pdf_doc_close(pdf_doc_t *doc) {
    if (doc->obj_cache) {
        for (int i = 0; i < doc->xref_count; i++)
            pdf_obj_free(doc->obj_cache[i]);
        free(doc->obj_cache);
    }
    free(doc->xref);
    pdf_obj_free(doc->trailer);
    memset(doc, 0, sizeof(pdf_doc_t));
}

/* Get pages root */
static pdf_obj_t *pdf_get_pages_root(pdf_doc_t *doc) {
    pdf_obj_t *root = pdf_resolve(doc, dict_get(doc->trailer, "Root"));
    if (!root) return NULL;
    return pdf_resolve(doc, dict_get(root, "Pages"));
}

/* ======================== Public API ======================== */

int sw_pdf_extract_text(const uint8_t *data, size_t len, char **out_text, size_t *out_len) {
    return sw_pdf_extract_pages(data, len, NULL, out_text, out_len);
}

int sw_pdf_extract_pages(const uint8_t *data, size_t len, const int *pages,
                         char **out_text, size_t *out_len) {
    pdf_doc_t doc;
    int rc = pdf_doc_open(&doc, data, len);
    if (rc != SW_PDF_OK) { *out_text = NULL; *out_len = 0; return rc; }

    /* Collect all pages */
    pdf_obj_t *pages_root = pdf_get_pages_root(&doc);
    if (!pages_root) { pdf_doc_close(&doc); *out_text = NULL; *out_len = 0; return SW_PDF_ERR_PARSE; }

    pdf_obj_t **page_arr = (pdf_obj_t **)calloc(PDF_MAX_PAGES, sizeof(pdf_obj_t *));
    pdf_obj_t **page_res = (pdf_obj_t **)calloc(PDF_MAX_PAGES, sizeof(pdf_obj_t *));
    if (!page_arr || !page_res) {
        free(page_arr); free(page_res);
        pdf_doc_close(&doc); *out_text = NULL; *out_len = 0; return SW_PDF_ERR_ALLOC;
    }
    int page_count = 0;
    collect_pages(&doc, pages_root, page_arr, page_res, &page_count, PDF_MAX_PAGES, NULL);

    /* Initialize text context */
    pdf_text_ctx_t ctx;
    text_ctx_init(&ctx);
    if (!ctx.buf) {
        free(page_arr); free(page_res);
        pdf_doc_close(&doc); *out_text = NULL; *out_len = 0; return SW_PDF_ERR_ALLOC;
    }

    /* Extract selected pages */
    if (pages) {
        /* Extract specific pages (0-indexed, -1 terminated) */
        for (int i = 0; pages[i] >= 0; i++) {
            int pi = pages[i];
            if (pi >= 0 && pi < page_count) {
                if (ctx.len > 0) buf_append_char(&ctx, '\n');
                extract_page_text(&doc, page_arr[pi], &ctx, page_res[pi]);
            }
        }
    } else {
        /* Extract all pages */
        for (int i = 0; i < page_count; i++) {
            if (ctx.len > 0) buf_append_char(&ctx, '\n');
            extract_page_text(&doc, page_arr[i], &ctx, page_res[i]);
        }
    }

    *out_text = ctx.buf;
    *out_len = ctx.len;

    free(page_arr);
    free(page_res);
    pdf_doc_close(&doc);
    return SW_PDF_OK;
}

int sw_pdf_page_count(const uint8_t *data, size_t len, int *out_count) {
    pdf_doc_t doc;
    int rc = pdf_doc_open(&doc, data, len);
    if (rc != SW_PDF_OK) { *out_count = 0; return rc; }

    pdf_obj_t *pages_root = pdf_get_pages_root(&doc);
    if (pages_root) {
        pdf_obj_t *cnt = dict_get(pages_root, "Count");
        *out_count = cnt ? (int)obj_int(cnt) : 0;
    } else {
        *out_count = 0;
    }

    pdf_doc_close(&doc);
    return SW_PDF_OK;
}

int sw_pdf_metadata(const uint8_t *data, size_t len, sw_pdf_meta_t *out) {
    memset(out, 0, sizeof(sw_pdf_meta_t));

    pdf_doc_t doc;
    int rc = pdf_doc_open(&doc, data, len);
    if (rc != SW_PDF_OK) return rc;

    pdf_obj_t *info = pdf_resolve(&doc, dict_get(doc.trailer, "Info"));
    if (info && info->type == PDF_OBJ_DICT) {
        pdf_obj_t *o;

        if ((o = dict_get(info, "Title")) && o->type == PDF_OBJ_STRING)
            out->title = strndup(o->v.string.data, o->v.string.len);
        if ((o = dict_get(info, "Author")) && o->type == PDF_OBJ_STRING)
            out->author = strndup(o->v.string.data, o->v.string.len);
        if ((o = dict_get(info, "Subject")) && o->type == PDF_OBJ_STRING)
            out->subject = strndup(o->v.string.data, o->v.string.len);
        if ((o = dict_get(info, "Creator")) && o->type == PDF_OBJ_STRING)
            out->creator = strndup(o->v.string.data, o->v.string.len);
        if ((o = dict_get(info, "CreationDate")) && o->type == PDF_OBJ_STRING)
            out->creation_date = strndup(o->v.string.data, o->v.string.len);
    }

    pdf_doc_close(&doc);
    return SW_PDF_OK;
}

void sw_pdf_meta_free(sw_pdf_meta_t *meta) {
    if (!meta) return;
    free(meta->title);
    free(meta->author);
    free(meta->subject);
    free(meta->creator);
    free(meta->creation_date);
    memset(meta, 0, sizeof(sw_pdf_meta_t));
}

int sw_pdf_extract_structured(const uint8_t *data, size_t len, int page_idx,
                              sw_pdf_text_block_t **out_blocks, int *out_count) {
    /* For now, extract text as a single block per page */
    int pages_sel[2] = { page_idx, -1 };
    char *text = NULL;
    size_t text_len = 0;

    int rc = sw_pdf_extract_pages(data, len, pages_sel, &text, &text_len);
    if (rc != SW_PDF_OK) { *out_blocks = NULL; *out_count = 0; return rc; }

    if (text_len == 0 || !text) {
        free(text);
        *out_blocks = NULL;
        *out_count = 0;
        return SW_PDF_OK;
    }

    sw_pdf_text_block_t *block = (sw_pdf_text_block_t *)calloc(1, sizeof(sw_pdf_text_block_t));
    if (!block) { free(text); *out_blocks = NULL; *out_count = 0; return SW_PDF_ERR_ALLOC; }

    block->text = text;
    block->x = 0; block->y = 0;
    block->w = 612; block->h = 792; /* default US Letter */
    block->font_size = 12;

    *out_blocks = block;
    *out_count = 1;
    return SW_PDF_OK;
}

void sw_pdf_blocks_free(sw_pdf_text_block_t *blocks, int count) {
    if (!blocks) return;
    for (int i = 0; i < count; i++)
        free(blocks[i].text);
    free(blocks);
}
