/*
 * SwarmRT Compiler: Obfuscation Passes
 *
 * Post-processing on generated C code:
 *   1. XOR string encoding (key 0xa7) — hides string literals
 *   2. Symbol mangling (FNV-1a hash) — hides function names
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "swarmrt_codegen.h"

#define XOR_KEY 0xa7
#define MAX_STRINGS 512
#define MAX_STR_LEN 256

/* =========================================================================
 * XOR String Encoding
 * ========================================================================= */

typedef struct {
    char original[MAX_STR_LEN];
    uint8_t encoded[MAX_STR_LEN];
    int len;
    int id;
} xor_string_t;

static xor_string_t xor_strings[MAX_STRINGS];
static int nxor_strings;

/* FNV-1a hash for symbol mangling */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Check if a string is already recorded */
static int find_xor_string(const char *s) {
    for (int i = 0; i < nxor_strings; i++)
        if (strcmp(xor_strings[i].original, s) == 0) return i;
    return -1;
}

/* Record a new string for XOR encoding */
static int add_xor_string(const char *s) {
    int idx = find_xor_string(s);
    if (idx >= 0) return idx;
    if (nxor_strings >= MAX_STRINGS) return -1;

    xor_string_t *xs = &xor_strings[nxor_strings];
    xs->id = nxor_strings;
    strncpy(xs->original, s, MAX_STR_LEN - 1);
    xs->len = (int)strlen(s);
    for (int i = 0; i < xs->len; i++)
        xs->encoded[i] = (uint8_t)s[i] ^ XOR_KEY;
    return nxor_strings++;
}

/* Extract string from a pattern like sw_val_atom("...") or sw_val_string("...")
 * or strcmp(..., "..."). Returns pointer past the closing quote, or NULL. */
static const char *extract_quoted(const char *p, char *out, int outsz) {
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\' && *(p + 1)) { out[i++] = *(p + 1); p += 2; }
        else out[i++] = *p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* Generate XOR decoder function and encoded string arrays */
static void emit_xor_preamble(FILE *out) {
    fprintf(out,
        "/* XOR string decoder */\n"
        "static char *_xd(const uint8_t *e, int len) {\n"
        "    static __thread char _xb[4][256];\n"
        "    static __thread int _xi = 0;\n"
        "    char *b = _xb[_xi++ & 3];\n"
        "    for (int i = 0; i < len && i < 255; i++) b[i] = e[i] ^ 0xa7;\n"
        "    b[len < 255 ? len : 255] = 0;\n"
        "    return b;\n"
        "}\n\n");

    for (int i = 0; i < nxor_strings; i++) {
        xor_string_t *xs = &xor_strings[i];
        fprintf(out, "static const uint8_t _xs%d[] = {", xs->id);
        for (int j = 0; j < xs->len; j++) {
            if (j) fprintf(out, ",");
            fprintf(out, "0x%02x", xs->encoded[j]);
        }
        fprintf(out, "}; /* %d bytes */\n", xs->len);
    }
    fprintf(out, "\n");
}

/* Apply XOR encoding to all string literals in generated code.
 * Scans for sw_val_atom("..."), sw_val_string("..."), strcmp(...,"..."),
 * and printf format strings that contain meaningful text. */
static char *apply_xor_strings(const char *code) {
    nxor_strings = 0;

    /* First pass: find all strings to encode */
    const char *p = code;
    while (*p) {
        /* Look for sw_val_atom(" or sw_val_string(" */
        if ((strncmp(p, "sw_val_atom(\"", 13) == 0) ||
            (strncmp(p, "sw_val_string(\"", 15) == 0)) {
            const char *qstart = strchr(p, '"');
            if (qstart) {
                char str[MAX_STR_LEN];
                extract_quoted(qstart, str, sizeof(str));
                if (strlen(str) > 0 && strcmp(str, "ok") != 0 &&
                    strcmp(str, "true") != 0 && strcmp(str, "false") != 0)
                    add_xor_string(str);
            }
        }
        /* Also catch strcmp(..., "...") */
        if (strncmp(p, "strcmp(", 6) == 0) {
            const char *q = strchr(p + 6, '"');
            if (q && q < p + 200) {
                char str[MAX_STR_LEN];
                extract_quoted(q, str, sizeof(str));
                if (strlen(str) > 0)
                    add_xor_string(str);
            }
        }
        p++;
    }

    if (nxor_strings == 0) return strdup(code);

    /* Second pass: build output with replacements */
    char *result = NULL;
    size_t rlen = 0;
    FILE *out = open_memstream(&result, &rlen);

    /* Find where to insert XOR preamble (after the #include block) */
    p = code;
    const char *insert_point = NULL;
    while (*p) {
        if (*p == '\n' && *(p + 1) != '#' && *(p + 1) != '\n' && insert_point == NULL) {
            if (p > code + 10) insert_point = p + 1;
        }
        if (strncmp(p, "/* === Forward", 14) == 0 ||
            strncmp(p, "static sw_val_t *_op_", 21) == 0) {
            insert_point = p;
            break;
        }
        p++;
    }
    if (!insert_point) insert_point = strstr(code, "static ");

    /* Write code up to insert point */
    if (insert_point) {
        fwrite(code, 1, insert_point - code, out);
        emit_xor_preamble(out);
        p = insert_point;
    } else {
        p = code;
    }

    /* Write rest of code with string replacements */
    while (*p) {
        int replaced = 0;

        /* Check for sw_val_atom("...") */
        if (strncmp(p, "sw_val_atom(\"", 13) == 0) {
            char str[MAX_STR_LEN];
            const char *qstart = p + 12;
            const char *after = extract_quoted(qstart, str, sizeof(str));
            int idx = find_xor_string(str);
            if (idx >= 0 && after && *after == ')') {
                fprintf(out, "sw_val_atom(_xd(_xs%d, %d))", idx, xor_strings[idx].len);
                p = after + 1;
                replaced = 1;
            }
        }

        /* Check for sw_val_string("...") */
        if (!replaced && strncmp(p, "sw_val_string(\"", 15) == 0) {
            char str[MAX_STR_LEN];
            const char *qstart = p + 14;
            const char *after = extract_quoted(qstart, str, sizeof(str));
            int idx = find_xor_string(str);
            if (idx >= 0 && after && *after == ')') {
                fprintf(out, "sw_val_string(_xd(_xs%d, %d))", idx, xor_strings[idx].len);
                p = after + 1;
                replaced = 1;
            }
        }

        /* Check for strcmp(..., "...") patterns */
        if (!replaced && strncmp(p, "strcmp(", 6) == 0) {
            /* Find the quoted string argument */
            const char *q = p + 6;
            const char *comma = strchr(q, ',');
            if (comma) {
                /* Skip to the string arg */
                const char *qs = comma + 1;
                while (*qs == ' ') qs++;
                if (*qs == '"') {
                    char str[MAX_STR_LEN];
                    const char *after = extract_quoted(qs, str, sizeof(str));
                    int idx = find_xor_string(str);
                    if (idx >= 0 && after) {
                        /* Write: strcmp(<first_arg>, _xd(...)) */
                        fwrite(p, 1, qs - p, out);
                        fprintf(out, "_xd(_xs%d, %d)", idx, xor_strings[idx].len);
                        p = after;
                        replaced = 1;
                    }
                }
            }
        }

        if (!replaced) {
            fputc(*p, out);
            p++;
        }
    }

    fclose(out);
    return result;
}

/* =========================================================================
 * Symbol Mangling
 * ========================================================================= */

static char *apply_symbol_mangle(const char *code, const char *mod_name,
                                  const char **func_names, int nfuncs) {
    char *result = strdup(code);

    for (int i = 0; i < nfuncs; i++) {
        /* Build original name: ModName_funcname */
        char orig[256];
        snprintf(orig, sizeof(orig), "%s_%s", mod_name, func_names[i]);

        /* Build mangled name: _f_0xHASH */
        char mangled[64];
        snprintf(mangled, sizeof(mangled), "_f_0x%08x", fnv1a(orig));

        /* Replace all occurrences */
        char *p;
        while ((p = strstr(result, orig)) != NULL) {
            /* Make sure it's a word boundary (not part of a longer identifier) */
            int orig_len = (int)strlen(orig);
            char after = p[orig_len];
            if (after != '(' && after != ')' && after != ';' && after != ',' &&
                after != ' ' && after != '\n' && after != '\0' && after != '[' &&
                after != ']' && after != '_') {
                /* Part of a longer name — skip by advancing past this occurrence */
                /* We need a different approach to avoid infinite loop */
                break;
            }

            int mangled_len = (int)strlen(mangled);
            int result_len = (int)strlen(result);
            int new_len = result_len - orig_len + mangled_len;
            char *new_result = malloc(new_len + 1);
            int offset = (int)(p - result);
            memcpy(new_result, result, offset);
            memcpy(new_result + offset, mangled, mangled_len);
            memcpy(new_result + offset + mangled_len, p + orig_len,
                   result_len - offset - orig_len + 1);
            free(result);
            result = new_result;
        }
    }

    /* Also mangle spawn trampoline names that reference module functions */
    /* _sp0_entry, _sp0_t etc. are already opaque, so we leave them */

    return result;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

char *sw_obfuscate(const char *code, const char *mod_name,
                   const char **func_names, int nfuncs) {
    if (!code) return NULL;

    /* Pass 1: XOR encode string literals */
    char *pass1 = apply_xor_strings(code);
    if (!pass1) return NULL;

    /* Pass 2: Symbol mangling */
    char *pass2 = apply_symbol_mangle(pass1, mod_name, func_names, nfuncs);
    free(pass1);

    return pass2;
}
