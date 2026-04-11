/*
 * swarmrt-wrap — PTY wrapper for Claude Code with scheduled wake injection.
 *
 * Usage:  swarmrt-wrap <command> [args...]
 * Typical:
 *   swarmrt-wrap claude
 *   swarmrt-wrap claude --resume
 *   alias claude='swarmrt-wrap claude'
 *
 * Owns a PTY pair, forks the target command (claude), relays stdin/stdout
 * bidirectionally, and on a 5-second tick checks .swarmrt/wakes.json in
 * the current working directory for due wakes. When one fires, it writes
 * the wake's prompt + \r to the PTY master — exactly as if the user had
 * typed it into the terminal.
 *
 * State files under .swarmrt/:
 *   wakes.json        config + runtime state (written by MCP and wrapper)
 *   wake_queue.jsonl  one-shot injection queue (written by MCP wake_fire_now)
 *
 * This is the delivery engine for the swarmrt-mcp wake_* tools.
 * Without it, wakes stay in wakes.json but never fire.
 *
 * Sections labeled "DUPLICATED" are lifted verbatim from mcp/swarmrt_mcp.c.
 * Keep them in sync if either side changes.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <termios.h>
#include <ctype.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#else
#include <pty.h>
#endif

#define WAKE_TICK_SEC   5   /* how often we check for due wakes */
#define IDLE_GRACE_SEC  5   /* defer injection if user typed within this window */
#define MAX_WAKES       64

/* ================================================================
 * DUPLICATED: Minimal JSON parser (source: mcp/swarmrt_mcp.c §2)
 * ================================================================ */

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_INT, JSON_FLOAT, JSON_STRING,
    JSON_ARRAY, JSON_OBJECT
} json_type_t;

typedef struct json_node {
    json_type_t type;
    char *key;
    union {
        bool bval;
        int64_t ival;
        double fval;
        char *sval;
        struct {
            struct json_node **items;
            int count;
            int cap;
        } children;
    };
} json_node_t;

#define JSON_MAX_DEPTH 64
#define JSON_MAX_STRING (1 * 1024 * 1024)

static json_node_t *json_parse_depth(const char **p, int depth);
static json_node_t *json_parse(const char **p) { return json_parse_depth(p, 0); }

static void json_free(json_node_t *n) {
    if (!n) return;
    free(n->key);
    if (n->type == JSON_STRING) free(n->sval);
    if (n->type == JSON_ARRAY || n->type == JSON_OBJECT) {
        for (int i = 0; i < n->children.count; i++) json_free(n->children.items[i]);
        free(n->children.items);
    }
    free(n);
}

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static json_node_t *json_new(json_type_t type) {
    json_node_t *n = calloc(1, sizeof(json_node_t));
    n->type = type;
    return n;
}

static void json_add_child(json_node_t *parent, json_node_t *child) {
    if (parent->children.count >= parent->children.cap) {
        parent->children.cap = parent->children.cap ? parent->children.cap * 2 : 8;
        parent->children.items = realloc(parent->children.items,
            parent->children.cap * sizeof(json_node_t *));
    }
    parent->children.items[parent->children.count++] = child;
}

static char *json_parse_string_raw(const char **p) {
    if (**p != '"') return NULL;
    (*p)++;
    size_t len = 0, cap = 256;
    char *buf = malloc(cap);
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            char c = **p;
            switch (c) {
                case '"': case '\\': case '/': buf[len++] = c; break;
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'f': buf[len++] = '\f'; break;
                case 'u': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 4 && (*p)[1]; i++) {
                        (*p)++;
                        char h = **p;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                        else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                    }
                    if (cp < 0x80) buf[len++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: buf[len++] = c; break;
            }
        } else {
            buf[len++] = **p;
        }
        if (len >= cap - 4) {
            if (cap >= JSON_MAX_STRING) break;
            cap *= 2;
            if (cap > JSON_MAX_STRING) cap = JSON_MAX_STRING;
            buf = realloc(buf, cap);
        }
        (*p)++;
    }
    if (**p == '"') (*p)++;
    buf[len] = '\0';
    return buf;
}

static json_node_t *json_parse_depth(const char **p, int depth) {
    if (depth > JSON_MAX_DEPTH) return NULL;
    skip_ws(p);
    if (!**p) return NULL;

    if (**p == '"') {
        json_node_t *n = json_new(JSON_STRING);
        n->sval = json_parse_string_raw(p);
        return n;
    }
    if (**p == '{') {
        (*p)++;
        json_node_t *n = json_new(JSON_OBJECT);
        skip_ws(p);
        if (**p == '}') { (*p)++; return n; }
        while (1) {
            skip_ws(p);
            char *key = json_parse_string_raw(p);
            skip_ws(p);
            if (**p == ':') (*p)++;
            json_node_t *val = json_parse_depth(p, depth + 1);
            if (val) { val->key = key; json_add_child(n, val); }
            else free(key);
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            break;
        }
        return n;
    }
    if (**p == '[') {
        (*p)++;
        json_node_t *n = json_new(JSON_ARRAY);
        skip_ws(p);
        if (**p == ']') { (*p)++; return n; }
        while (1) {
            json_node_t *val = json_parse_depth(p, depth + 1);
            if (val) json_add_child(n, val);
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            break;
        }
        return n;
    }
    if (strncmp(*p, "true", 4) == 0)  { *p += 4; json_node_t *n = json_new(JSON_BOOL); n->bval = true; return n; }
    if (strncmp(*p, "false", 5) == 0) { *p += 5; json_node_t *n = json_new(JSON_BOOL); n->bval = false; return n; }
    if (strncmp(*p, "null", 4) == 0)  { *p += 4; return json_new(JSON_NULL); }
    {
        char *end;
        int64_t iv = strtoll(*p, &end, 10);
        if (end != *p) {
            if (*end == '.' || *end == 'e' || *end == 'E') {
                double fv = strtod(*p, &end);
                json_node_t *n = json_new(JSON_FLOAT);
                n->fval = fv;
                *p = end;
                return n;
            }
            json_node_t *n = json_new(JSON_INT);
            n->ival = iv;
            *p = end;
            return n;
        }
    }
    return NULL;
}

static json_node_t *json_get(json_node_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->children.count; i++) {
        if (obj->children.items[i]->key && strcmp(obj->children.items[i]->key, key) == 0)
            return obj->children.items[i];
    }
    return NULL;
}

static const char *json_get_str(json_node_t *obj, const char *key) {
    json_node_t *n = json_get(obj, key);
    return (n && n->type == JSON_STRING) ? n->sval : NULL;
}

static int64_t json_get_int(json_node_t *obj, const char *key, int64_t def) {
    json_node_t *n = json_get(obj, key);
    return (n && n->type == JSON_INT) ? n->ival : def;
}

static bool json_get_bool(json_node_t *obj, const char *key, bool def) {
    json_node_t *n = json_get(obj, key);
    return (n && n->type == JSON_BOOL) ? n->bval : def;
}

/* ================================================================
 * DUPLICATED: Cron parser (source: mcp/swarmrt_mcp.c §7.6)
 * ================================================================ */

typedef struct {
    uint64_t minute;
    uint64_t hour;
    uint32_t dom;
    uint32_t month;
    uint8_t  dow;
    bool     dom_star;
    bool     dow_star;
} cron_t;

static bool cron_parse_field(const char *s, int lo_min, int hi_max, uint64_t *mask) {
    *mask = 0;
    char buf[256];
    size_t len = strlen(s);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, s, len + 1);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int step = 1;
        char *slash = strchr(tok, '/');
        if (slash) {
            *slash = 0;
            step = atoi(slash + 1);
            if (step < 1) return false;
        }
        int lo, hi;
        if (strcmp(tok, "*") == 0) { lo = lo_min; hi = hi_max; }
        else {
            char *dash = strchr(tok, '-');
            if (dash) { *dash = 0; lo = atoi(tok); hi = atoi(dash + 1); }
            else      { lo = atoi(tok); hi = lo; }
        }
        if (lo < lo_min || hi > hi_max || lo > hi) return false;
        for (int i = lo; i <= hi; i += step) *mask |= (1ULL << i);
    }
    return *mask != 0;
}

static bool cron_parse(const char *expr, cron_t *c) {
    memset(c, 0, sizeof(*c));
    char buf[512];
    size_t len = strlen(expr);
    if (len == 0 || len >= sizeof(buf)) return false;
    memcpy(buf, expr, len + 1);

    char *fields[5] = {0};
    int fcount = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok && fcount < 5;
         tok = strtok_r(NULL, " \t", &save)) {
        fields[fcount++] = tok;
    }
    if (fcount != 5) return false;

    c->dom_star = (strcmp(fields[2], "*") == 0);
    c->dow_star = (strcmp(fields[4], "*") == 0);

    uint64_t mask;
    if (!cron_parse_field(fields[0], 0, 59, &mask)) return false;
    c->minute = mask;
    if (!cron_parse_field(fields[1], 0, 23, &mask)) return false;
    c->hour = mask;
    if (!cron_parse_field(fields[2], 1, 31, &mask)) return false;
    c->dom = (uint32_t)mask;
    if (!cron_parse_field(fields[3], 1, 12, &mask)) return false;
    c->month = (uint32_t)mask;
    if (!cron_parse_field(fields[4], 0, 6, &mask)) return false;
    c->dow = (uint8_t)mask;
    return true;
}

static time_t cron_next_fire(const cron_t *c, time_t from) {
    time_t t = from + 60 - (from % 60);
    if (t <= from) t = from + 60;
    struct tm tm;
    for (int guard = 0; guard < 4 * 366 * 1440; guard++) {
        localtime_r(&t, &tm);
        if (!(c->month & (1U << (tm.tm_mon + 1)))) {
            tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_mon++;
            if (tm.tm_mon > 11) { tm.tm_mon = 0; tm.tm_year++; }
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }
        bool dom_match = (c->dom & (1U << tm.tm_mday)) != 0;
        bool dow_match = (c->dow & (1U << tm.tm_wday)) != 0;
        bool day_ok;
        if (c->dom_star && c->dow_star)      day_ok = true;
        else if (c->dom_star)                day_ok = dow_match;
        else if (c->dow_star)                day_ok = dom_match;
        else                                  day_ok = dom_match || dow_match;
        if (!day_ok) {
            tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_mday++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }
        if (!(c->hour & (1ULL << tm.tm_hour))) {
            tm.tm_min = 0; tm.tm_sec = 0;
            tm.tm_hour++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }
        if (!(c->minute & (1ULL << tm.tm_min))) {
            tm.tm_sec = 0;
            tm.tm_min++;
            t = mktime(&tm);
            if (t == -1) return 0;
            continue;
        }
        return t;
    }
    return 0;
}

/* ================================================================
 * Wake state (wrapper-local view of wakes.json)
 * ================================================================ */

typedef struct {
    int      id;
    char    *name;
    char    *cron_expr;
    char    *prompt;
    cron_t   cron;
    bool     enabled;
    int64_t  created_at;
    int64_t  last_fired_at;
    int      fire_count;
} wake_t;

static wake_t  g_wakes[MAX_WAKES];
static int     g_wake_count   = 0;
static int     g_next_id      = 1;
static char    g_swarmrt_dir[4096];
static char    g_wakes_path[4096];
static char    g_queue_path[4096];
static int64_t g_wakes_mtime  = 0;
static bool    g_verbose      = false;

static void logmsg(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[swarmrt-wrap] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void wake_free(wake_t *w) {
    free(w->name); w->name = NULL;
    free(w->cron_expr); w->cron_expr = NULL;
    free(w->prompt); w->prompt = NULL;
}

static void wakes_clear(void) {
    for (int i = 0; i < g_wake_count; i++) wake_free(&g_wakes[i]);
    g_wake_count = 0;
}

static bool wakes_load_if_changed(void) {
    struct stat st;
    if (stat(g_wakes_path, &st) != 0) return false;
    if ((int64_t)st.st_mtime == g_wakes_mtime && g_wake_count > 0) return false;

    FILE *f = fopen(g_wakes_path, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return false; }
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    const char *p = buf;
    json_node_t *root = json_parse(&p);
    if (!root) { free(buf); return false; }

    wakes_clear();

    g_next_id = (int)json_get_int(root, "next_id", 1);
    json_node_t *arr = json_get(root, "wakes");
    if (arr && arr->type == JSON_ARRAY) {
        for (int i = 0; i < arr->children.count && g_wake_count < MAX_WAKES; i++) {
            json_node_t *w_n = arr->children.items[i];
            const char *cron_s = json_get_str(w_n, "cron");
            const char *prompt = json_get_str(w_n, "prompt");
            if (!cron_s || !prompt) continue;
            wake_t *w = &g_wakes[g_wake_count];
            memset(w, 0, sizeof(*w));
            w->id            = (int)json_get_int(w_n, "id", g_wake_count + 1);
            const char *name = json_get_str(w_n, "name");
            w->name          = (name && name[0]) ? strdup(name) : NULL;
            w->cron_expr     = strdup(cron_s);
            w->prompt        = strdup(prompt);
            w->enabled       = json_get_bool(w_n, "enabled", true);
            w->created_at    = json_get_int(w_n, "created_at", 0);
            w->last_fired_at = json_get_int(w_n, "last_fired_at", 0);
            w->fire_count    = (int)json_get_int(w_n, "fire_count", 0);
            if (!cron_parse(w->cron_expr, &w->cron)) { wake_free(w); continue; }
            g_wake_count++;
        }
    }
    json_free(root);
    free(buf);

    g_wakes_mtime = (int64_t)st.st_mtime;
    logmsg("loaded %d wake(s) from %s", g_wake_count, g_wakes_path);
    return true;
}

static void jesc(FILE *f, const char *s) {
    fputc('"', f);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
                case '"':  fputs("\\\"", f); break;
                case '\\': fputs("\\\\", f); break;
                case '\n': fputs("\\n", f); break;
                case '\r': fputs("\\r", f); break;
                case '\t': fputs("\\t", f); break;
                case '\b': fputs("\\b", f); break;
                case '\f': fputs("\\f", f); break;
                default:
                    if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                    else fputc(*p, f);
            }
        }
    }
    fputc('"', f);
}

/* Atomic save of wakes.json preserving all known fields (incl. name). */
static void wakes_save(void) {
    char tmp_path[4100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_wakes_path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f, "{\"next_id\":%d,\"wakes\":[", g_next_id);
    for (int i = 0; i < g_wake_count; i++) {
        wake_t *w = &g_wakes[i];
        if (i > 0) fputc(',', f);
        fprintf(f, "{\"id\":%d,\"name\":", w->id);
        if (w->name) jesc(f, w->name); else fputs("null", f);
        fprintf(f, ",\"cron\":"); jesc(f, w->cron_expr);
        fprintf(f, ",\"prompt\":"); jesc(f, w->prompt);
        fprintf(f, ",\"enabled\":%s,\"created_at\":%lld,\"last_fired_at\":%lld,\"fire_count\":%d}",
                w->enabled ? "true" : "false",
                (long long)w->created_at,
                (long long)w->last_fired_at,
                w->fire_count);
    }
    fprintf(f, "]}");
    fclose(f);

    rename(tmp_path, g_wakes_path);
    struct stat st;
    if (stat(g_wakes_path, &st) == 0) g_wakes_mtime = (int64_t)st.st_mtime;
}

/* ================================================================
 * Terminal + PTY + signals
 * ================================================================ */

static pid_t g_child_pid    = 0;
static int   g_pty_master   = -1;
static struct termios g_orig_termios;
static bool  g_termios_saved = false;

static void restore_tty(void) {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = false;
    }
}

static void on_exit_cleanup(void) { restore_tty(); }

static volatile sig_atomic_t g_pending_sig = 0;

static void on_signal(int sig) {
    g_pending_sig = sig;
    if (g_child_pid > 0) kill(g_child_pid, sig);
}

static void forward_winsize(void) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && g_pty_master >= 0)
        ioctl(g_pty_master, TIOCSWINSZ, &ws);
}

static void on_winch(int sig) { (void)sig; forward_winsize(); }

static int enter_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return -1;
    g_termios_saved = true;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return -1;
    return 0;
}

/* ================================================================
 * Injection + tick
 * ================================================================ */

static void inject(int fd, const char *prompt) {
    if (!prompt) return;
    size_t n = strlen(prompt);
    ssize_t w = write(fd, prompt, n);
    (void)w;
    w = write(fd, "\r", 1);
    (void)w;
    logmsg("injected: %.60s%s", prompt, n > 60 ? "..." : "");
}

static void drain_queue(int master_fd) {
    FILE *f = fopen(g_queue_path, "r");
    if (!f) return;

    char line[16384];
    int injected = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        json_node_t *n = json_parse(&p);
        if (!n) continue;
        const char *prompt = json_get_str(n, "prompt");
        if (prompt) { inject(master_fd, prompt); injected++; }
        json_free(n);
    }
    fclose(f);
    if (injected > 0) {
        unlink(g_queue_path);
        logmsg("drained queue: %d injection(s)", injected);
    }
}

static void process_tick(int master_fd, time_t now) {
    /* 1. Drain manual queue unconditionally (user-initiated). */
    drain_queue(master_fd);

    /* 2. Pick up any config changes from disk. */
    wakes_load_if_changed();

    /* 3. Fire at most one due cron wake per tick. */
    for (int i = 0; i < g_wake_count; i++) {
        wake_t *w = &g_wakes[i];
        if (!w->enabled) continue;
        time_t base = (w->last_fired_at > w->created_at) ? w->last_fired_at : w->created_at;
        if (base == 0) base = now - 60; /* fresh wake: compute from ~now */
        time_t next = cron_next_fire(&w->cron, base);
        if (next > 0 && next <= now) {
            inject(master_fd, w->prompt);
            w->last_fired_at = now;
            w->fire_count++;
            wakes_save();
            break; /* only one per tick */
        }
    }
}

/* ================================================================
 * Main
 * ================================================================ */

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-v] <command> [args...]\n"
        "\n"
        "Wraps <command> (typically claude) in a PTY and injects\n"
        "scheduled wake prompts from .swarmrt/wakes.json (in cwd)\n"
        "into the session on their cron schedule.\n"
        "\n"
        "Typical:\n"
        "  %s claude\n"
        "  %s claude --resume\n"
        "  alias claude='%s claude'\n"
        "\n"
        "Flags:\n"
        "  -v   verbose logging to stderr\n",
        argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v") == 0) { g_verbose = true; argi++; }
        else if (strcmp(argv[argi], "--") == 0) { argi++; break; }
        else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
            usage(argv[0]); return 0;
        } else break;
    }
    if (argi >= argc) { usage(argv[0]); return 1; }

    /* Set up .swarmrt paths from cwd */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }
    snprintf(g_swarmrt_dir, sizeof(g_swarmrt_dir), "%s/.swarmrt", cwd);
    snprintf(g_wakes_path, sizeof(g_wakes_path), "%s/wakes.json", g_swarmrt_dir);
    snprintf(g_queue_path, sizeof(g_queue_path), "%s/wake_queue.jsonl", g_swarmrt_dir);
    logmsg("cwd=%s swarmrt=%s", cwd, g_swarmrt_dir);

    /* Get current window size to pass to the child's PTY */
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
        ws.ws_row = 24; ws.ws_col = 80; ws.ws_xpixel = 0; ws.ws_ypixel = 0;
    }

    /* Fork with a new PTY */
    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return 1; }

    if (pid == 0) {
        /* Child — exec the target command on the slave side */
        execvp(argv[argi], &argv[argi]);
        fprintf(stderr, "swarmrt-wrap: exec %s: %s\n", argv[argi], strerror(errno));
        _exit(127);
    }

    /* Parent */
    g_child_pid  = pid;
    g_pty_master = master_fd;

    atexit(on_exit_cleanup);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGWINCH, on_winch);
    signal(SIGPIPE, SIG_IGN);

    enter_raw_mode();  /* best-effort; if stdin isn't a tty, proceed without */

    wakes_load_if_changed();

    time_t last_user_activity = time(NULL);
    time_t last_tick          = 0;
    char   iobuf[16384];
    bool   child_alive = true;

    while (child_alive) {
        /* Non-blocking child-exit check */
        int wstatus;
        pid_t r = waitpid(g_child_pid, &wstatus, WNOHANG);
        if (r == g_child_pid) { child_alive = false; break; }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(master_fd, &rfds);
        int maxfd = (master_fd > STDIN_FILENO) ? master_fd : STDIN_FILENO;

        /* 1-second select tick — small so we respond to child exit / winch / wakes promptly. */
        struct timeval tv = {1, 0};
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        time_t now = time(NULL);

        /* user -> child */
        if (n > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t nr = read(STDIN_FILENO, iobuf, sizeof(iobuf));
            if (nr > 0) {
                ssize_t nw = write(master_fd, iobuf, nr);
                (void)nw;
                last_user_activity = now;
            } else if (nr == 0) {
                break; /* stdin EOF */
            }
        }

        /* child -> user */
        if (n > 0 && FD_ISSET(master_fd, &rfds)) {
            ssize_t nr = read(master_fd, iobuf, sizeof(iobuf));
            if (nr > 0) {
                ssize_t nw = write(STDOUT_FILENO, iobuf, nr);
                (void)nw;
            } else if (nr <= 0) {
                break; /* pty closed */
            }
        }

        /* Wake tick */
        if (now - last_tick >= WAKE_TICK_SEC) {
            last_tick = now;
            if (now - last_user_activity >= IDLE_GRACE_SEC) {
                process_tick(master_fd, now);
            } else {
                logmsg("tick deferred: user active %lds ago", (long)(now - last_user_activity));
            }
        }
    }

    restore_tty();

    int wstatus = 0;
    waitpid(g_child_pid, &wstatus, 0);
    wakes_clear();
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
}
