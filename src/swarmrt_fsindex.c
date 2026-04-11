/*
 * SwarmRT Filesystem Search CLI (sws)
 *
 * Indexes file content and paths on local disk, searches instantly
 * using SwarmRT Search v2 BM25 and fuzzy engines.
 *
 * Usage:
 *   sws index [--config PATH] [--force] [PATHS...]
 *   sws search [--config PATH] [-n LIMIT] [-t bm25|fuzzy] QUERY
 *   sws info [--config PATH]
 *   sws watch [--config PATH] [--interval SECS]
 *
 * otonomy.ai
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #define sw_sleep(s) Sleep((s) * 1000)
  #define sw_lstat(p, s) stat(p, s)
  /* Simple fnmatch replacement for Windows (supports * and ? only) */
  static int fnmatch(const char *pat, const char *str, int flags) {
      (void)flags;
      while (*pat && *str) {
          if (*pat == '*') {
              pat++;
              if (!*pat) return 0;
              while (*str) { if (fnmatch(pat, str, 0) == 0) return 0; str++; }
              return 1;
          } else if (*pat == '?' || *pat == *str) {
              pat++; str++;
          } else return 1;
      }
      while (*pat == '*') pat++;
      return (*pat || *str) ? 1 : 0;
  }
#else
  #include <unistd.h>
  #include <signal.h>
  #include <fnmatch.h>
  #include <pwd.h>
  #define sw_sleep(s) sleep(s)
  #define sw_lstat(p, s) lstat(p, s)
#endif
#include "swarmrt_platform.h"

#include "swarmrt_search.h"

/* ── Configuration ─────────────────────────────────────────────── */

#define FSIDX_MAX_PATHS     64
#define FSIDX_MAX_EXCLUDES  128
#define FSIDX_DEFAULT_MAX_FILE_SIZE   (256 * 1024)
#define FSIDX_DEFAULT_CONTENT_PREVIEW 4096
#define FSIDX_MTIME_BUCKETS 65536
#define FSIDX_META_MAGIC    0x53574D31  /* "SWM1" */
#define FSIDX_DIR_STACK_CAP 4096

typedef struct {
    char *paths[FSIDX_MAX_PATHS];
    int   path_count;
    char *excludes[FSIDX_MAX_EXCLUDES];
    int   exclude_count;
    size_t max_file_size;
    size_t content_preview;
    char  *index_dir;
} fsidx_config_t;

/* ── Mtime Map ─────────────────────────────────────────────────── */

typedef struct fsidx_mtime_entry {
    uint64_t path_hash;
    int64_t  mtime;
    uint8_t  seen;
    struct fsidx_mtime_entry *next;
} fsidx_mtime_entry_t;

typedef struct {
    fsidx_mtime_entry_t *buckets[FSIDX_MTIME_BUCKETS];
    uint32_t count;
} fsidx_mtime_map_t;

/* ── Globals ───────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── FNV-1a 64-bit ─────────────────────────────────────────────── */

static uint64_t fnv1a_64(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ── Tilde Expansion ───────────────────────────────────────────── */

static char *expand_tilde(const char *path) {
    if (!path || path[0] != '~') return strdup(path ? path : "");
    const char *home = sw_homedir();
    if (!home) home = sw_tmpdir();
    size_t hlen = strlen(home);
    size_t plen = strlen(path + 1);
    char *out = malloc(hlen + plen + 1);
    memcpy(out, home, hlen);
    memcpy(out + hlen, path + 1, plen + 1);
    return out;
}

/* ── Config ────────────────────────────────────────────────────── */

static void config_init(fsidx_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_file_size = FSIDX_DEFAULT_MAX_FILE_SIZE;
    cfg->content_preview = FSIDX_DEFAULT_CONTENT_PREVIEW;
    cfg->index_dir = expand_tilde("~/.config/sws");
}

static void config_free(fsidx_config_t *cfg) {
    for (int i = 0; i < cfg->path_count; i++) free(cfg->paths[i]);
    for (int i = 0; i < cfg->exclude_count; i++) free(cfg->excludes[i]);
    free(cfg->index_dir);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')) *e-- = '\0';
    return s;
}

static int config_load(fsidx_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "path") == 0 && cfg->path_count < FSIDX_MAX_PATHS) {
            cfg->paths[cfg->path_count++] = expand_tilde(val);
        } else if (strcmp(key, "exclude") == 0 && cfg->exclude_count < FSIDX_MAX_EXCLUDES) {
            cfg->excludes[cfg->exclude_count++] = strdup(val);
        } else if (strcmp(key, "max_file_size") == 0) {
            cfg->max_file_size = (size_t)atol(val);
        } else if (strcmp(key, "content_preview") == 0) {
            cfg->content_preview = (size_t)atol(val);
        } else if (strcmp(key, "index_dir") == 0) {
            free(cfg->index_dir);
            cfg->index_dir = expand_tilde(val);
        }
    }
    fclose(f);
    return 0;
}

/* ── Mtime Map Operations ──────────────────────────────────────── */

static void mtime_init(fsidx_mtime_map_t *m) {
    memset(m, 0, sizeof(*m));
}

static void mtime_free(fsidx_mtime_map_t *m) {
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        fsidx_mtime_entry_t *e = m->buckets[i];
        while (e) {
            fsidx_mtime_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }
}

static fsidx_mtime_entry_t *mtime_get(fsidx_mtime_map_t *m, uint64_t hash) {
    uint32_t b = hash % FSIDX_MTIME_BUCKETS;
    for (fsidx_mtime_entry_t *e = m->buckets[b]; e; e = e->next) {
        if (e->path_hash == hash) return e;
    }
    return NULL;
}

static void mtime_set(fsidx_mtime_map_t *m, uint64_t hash, int64_t mtime) {
    fsidx_mtime_entry_t *e = mtime_get(m, hash);
    if (e) {
        e->mtime = mtime;
        e->seen = 1;
        return;
    }
    e = calloc(1, sizeof(*e));
    e->path_hash = hash;
    e->mtime = mtime;
    e->seen = 1;
    uint32_t b = hash % FSIDX_MTIME_BUCKETS;
    e->next = m->buckets[b];
    m->buckets[b] = e;
    m->count++;
}

static void mtime_clear_seen(fsidx_mtime_map_t *m) {
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        for (fsidx_mtime_entry_t *e = m->buckets[i]; e; e = e->next)
            e->seen = 0;
    }
}

static int mtime_save(fsidx_mtime_map_t *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t magic = FSIDX_META_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&m->count, 4, 1, f);
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        for (fsidx_mtime_entry_t *e = m->buckets[i]; e; e = e->next) {
            fwrite(&e->path_hash, 8, 1, f);
            fwrite(&e->mtime, 8, 1, f);
        }
    }
    fclose(f);
    return 0;
}

static int mtime_load(fsidx_mtime_map_t *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, count;
    if (fread(&magic, 4, 1, f) != 1 || magic != FSIDX_META_MAGIC) {
        fclose(f);
        return -1;
    }
    if (fread(&count, 4, 1, f) != 1) { fclose(f); return -1; }
    for (uint32_t i = 0; i < count; i++) {
        uint64_t hash;
        int64_t mtime;
        if (fread(&hash, 8, 1, f) != 1 || fread(&mtime, 8, 1, f) != 1) break;
        mtime_set(m, hash, mtime);
    }
    fclose(f);
    /* Clear seen flags — they were set by mtime_set during load */
    mtime_clear_seen(m);
    return 0;
}

/* ── Binary Detection ──────────────────────────────────────────── */

static int is_binary(const char *buf, size_t len) {
    size_t check = len < 512 ? len : 512;
    for (size_t i = 0; i < check; i++) {
        if (buf[i] == '\0') return 1;
    }
    return 0;
}

/* ── Exclusion Matching ────────────────────────────────────────── */

static int is_excluded(const char *basename, fsidx_config_t *cfg) {
    for (int i = 0; i < cfg->exclude_count; i++) {
        if (fnmatch(cfg->excludes[i], basename, 0) == 0) return 1;
    }
    return 0;
}

/* ── Directory Walker ──────────────────────────────────────────── */

typedef struct {
    char   *dirs[FSIDX_DIR_STACK_CAP];
    int     top;
} dir_stack_t;

static void dir_stack_push(dir_stack_t *s, const char *path) {
    if (s->top < FSIDX_DIR_STACK_CAP)
        s->dirs[s->top++] = strdup(path);
}

static char *dir_stack_pop(dir_stack_t *s) {
    return s->top > 0 ? s->dirs[--s->top] : NULL;
}

typedef void (*walk_cb)(const char *path, const struct stat *st, void *ctx);

static void walk_dirs(const char *root, fsidx_config_t *cfg, walk_cb cb, void *ctx) {
    dir_stack_t stack;
    stack.top = 0;
    dir_stack_push(&stack, root);

    char pathbuf[4096];
    char *dir;
    while ((dir = dir_stack_pop(&stack))) {
        DIR *d = opendir(dir);
        if (!d) { free(dir); continue; }

        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' ||
                 (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
                continue;

            if (is_excluded(ent->d_name, cfg)) continue;

            snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dir, ent->d_name);

            struct stat st;
            if (sw_lstat(pathbuf, &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                dir_stack_push(&stack, pathbuf);
            } else if (S_ISREG(st.st_mode)) {
                if ((size_t)st.st_size <= cfg->max_file_size) {
                    cb(pathbuf, &st, ctx);
                }
            }
        }
        closedir(d);
        free(dir);
    }
}

/* ── Index Context ─────────────────────────────────────────────── */

typedef struct {
    sws_index_t     *idx;
    fsidx_config_t  *cfg;
    fsidx_mtime_map_t *mtime;
    int force;
    uint32_t files_indexed;
    uint32_t files_skipped;
    uint32_t files_removed;
} index_ctx_t;

static void index_file_cb(const char *path, const struct stat *st, void *ctx) {
    index_ctx_t *ic = (index_ctx_t *)ctx;
    uint64_t doc_id = fnv1a_64(path, strlen(path));
    int64_t mtime = (int64_t)st->st_mtime;

    /* Check mtime for incremental skip */
    if (!ic->force) {
        fsidx_mtime_entry_t *me = mtime_get(ic->mtime, doc_id);
        if (me && me->mtime == mtime) {
            me->seen = 1;
            ic->files_skipped++;
            return;
        }
    }

    /* Read file content */
    FILE *f = fopen(path, "r");
    if (!f) return;

    size_t preview = ic->cfg->content_preview;
    char *buf = malloc(preview + 1);
    size_t nread = fread(buf, 1, preview, f);
    fclose(f);
    buf[nread] = '\0';

    /* Build document text: path\ncontent */
    size_t pathlen = strlen(path);
    int binary = is_binary(buf, nread);

    size_t doclen;
    char *doctext;
    if (binary) {
        /* Index path only for binary files */
        doclen = pathlen;
        doctext = malloc(doclen + 1);
        memcpy(doctext, path, pathlen + 1);
    } else {
        doclen = pathlen + 1 + nread;
        doctext = malloc(doclen + 1);
        memcpy(doctext, path, pathlen);
        doctext[pathlen] = '\n';
        memcpy(doctext + pathlen + 1, buf, nread);
        doctext[doclen] = '\0';
    }
    free(buf);

    /* Remove old version if exists, then add */
    sws_remove(ic->idx, doc_id);
    sws_add(ic->idx, doc_id, doctext, NULL, 0);
    free(doctext);

    mtime_set(ic->mtime, doc_id, mtime);
    ic->files_indexed++;
}

/* ── Collect Unseen (for deletion) ─────────────────────────────── */

static uint64_t *collect_unseen(fsidx_mtime_map_t *m, uint32_t *out_count) {
    /* Count unseen */
    uint32_t count = 0;
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        for (fsidx_mtime_entry_t *e = m->buckets[i]; e; e = e->next) {
            if (!e->seen) count++;
        }
    }
    if (count == 0) { *out_count = 0; return NULL; }

    uint64_t *ids = malloc(count * sizeof(uint64_t));
    uint32_t j = 0;
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        for (fsidx_mtime_entry_t *e = m->buckets[i]; e; e = e->next) {
            if (!e->seen) ids[j++] = e->path_hash;
        }
    }
    *out_count = count;
    return ids;
}

/* Remove unseen entries from the mtime map */
static void mtime_remove_unseen(fsidx_mtime_map_t *m) {
    for (uint32_t i = 0; i < FSIDX_MTIME_BUCKETS; i++) {
        fsidx_mtime_entry_t **pp = &m->buckets[i];
        while (*pp) {
            if (!(*pp)->seen) {
                fsidx_mtime_entry_t *del = *pp;
                *pp = del->next;
                free(del);
                m->count--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

/* ── Path Helpers ──────────────────────────────────────────────── */

static void ensure_dir(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void build_path(char *out, size_t sz, const char *dir, const char *file) {
    snprintf(out, sz, "%s/%s", dir, file);
}

/* ── Extract Path from Doc Text ────────────────────────────────── */

static const char *extract_path(const char *text, char *buf, size_t bufsz) {
    const char *nl = strchr(text, '\n');
    size_t len;
    if (nl) {
        len = (size_t)(nl - text);
    } else {
        len = strlen(text);
    }
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    return buf;
}

/* ── Extract Content Snippet ───────────────────────────────────── */

static const char *extract_snippet(const char *text, const char *query, char *buf, size_t bufsz) {
    const char *nl = strchr(text, '\n');
    if (!nl) { buf[0] = '\0'; return buf; }
    const char *content = nl + 1;
    size_t clen = strlen(content);

    /* Try to find query in content for context */
    const char *match = NULL;
    size_t qlen = strlen(query);
    /* Case-insensitive search for first query word */
    char first_word[256];
    size_t wlen = 0;
    for (size_t i = 0; i < qlen && i < sizeof(first_word) - 1; i++) {
        if (query[i] == ' ') break;
        first_word[wlen++] = query[i];
    }
    first_word[wlen] = '\0';

    if (wlen > 0) {
        for (size_t i = 0; i + wlen <= clen; i++) {
            int found = 1;
            for (size_t j = 0; j < wlen; j++) {
                char a = content[i + j];
                char b = first_word[j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { found = 0; break; }
            }
            if (found) { match = content + i; break; }
        }
    }

    size_t snippet_len = bufsz - 4; /* room for "..." */
    const char *start;
    if (match) {
        /* Center snippet around match */
        size_t offset = (size_t)(match - content);
        size_t half = snippet_len / 2;
        size_t soff = offset > half ? offset - half : 0;
        start = content + soff;
        size_t avail = clen - soff;
        if (avail > snippet_len) avail = snippet_len;
        /* Copy, replacing newlines with spaces */
        size_t i;
        for (i = 0; i < avail; i++) {
            buf[i] = (start[i] == '\n' || start[i] == '\r') ? ' ' : start[i];
        }
        buf[i] = '\0';
    } else {
        /* Show beginning */
        size_t avail = clen < snippet_len ? clen : snippet_len;
        for (size_t i = 0; i < avail; i++) {
            buf[i] = (content[i] == '\n' || content[i] == '\r') ? ' ' : content[i];
        }
        buf[avail] = '\0';
    }
    return buf;
}

/* ── Time Helpers ──────────────────────────────────────────────── */

static double time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ── Commands ──────────────────────────────────────────────────── */

static int cmd_index(fsidx_config_t *cfg, int force, int argc, char **argv) {
    /* Extra positional args override config paths */
    if (argc > 0) {
        for (int i = 0; i < cfg->path_count; i++) free(cfg->paths[i]);
        cfg->path_count = 0;
        for (int i = 0; i < argc && cfg->path_count < FSIDX_MAX_PATHS; i++) {
            cfg->paths[cfg->path_count++] = strdup(argv[i]);
        }
    }

    /* Default: index current directory */
    if (cfg->path_count == 0) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)))
            cfg->paths[cfg->path_count++] = strdup(cwd);
    }

    ensure_dir(cfg->index_dir);

    char idx_path[4096], meta_path[4096];
    build_path(idx_path, sizeof(idx_path), cfg->index_dir, "index.sws");
    build_path(meta_path, sizeof(meta_path), cfg->index_dir, "index.meta");

    /* Load or create index */
    sws_index_t *idx = NULL;
    fsidx_mtime_map_t mtime;
    mtime_init(&mtime);

    if (!force) {
        idx = sws_load(idx_path);
        if (idx) mtime_load(&mtime, meta_path);
    }
    if (!idx) {
        idx = sws_new(0); /* text-only, no vectors */
        force = 1; /* fresh index = index everything */
    }

    mtime_clear_seen(&mtime);

    index_ctx_t ic = {
        .idx = idx, .cfg = cfg, .mtime = &mtime,
        .force = force, .files_indexed = 0, .files_skipped = 0, .files_removed = 0
    };

    double t0 = time_ms();

    for (int i = 0; i < cfg->path_count; i++) {
        walk_dirs(cfg->paths[i], cfg, index_file_cb, &ic);
    }

    /* Remove deleted files */
    uint32_t del_count = 0;
    uint64_t *del_ids = collect_unseen(&mtime, &del_count);
    for (uint32_t i = 0; i < del_count; i++) {
        sws_remove(idx, del_ids[i]);
    }
    free(del_ids);
    ic.files_removed = del_count;
    mtime_remove_unseen(&mtime);

    double elapsed = time_ms() - t0;

    /* Save */
    sws_save(idx, idx_path);
    mtime_save(&mtime, meta_path);

    printf("Indexed %u files, skipped %u unchanged, removed %u deleted (%.1fms)\n",
           ic.files_indexed, ic.files_skipped, ic.files_removed, elapsed);
    printf("Total documents: %u\n", sws_count(idx));

    sws_free(idx);
    mtime_free(&mtime);
    return 0;
}

static int cmd_search(fsidx_config_t *cfg, const char *query, int limit, int use_fuzzy) {
    char idx_path[4096];
    build_path(idx_path, sizeof(idx_path), cfg->index_dir, "index.sws");

    sws_index_t *idx = sws_load(idx_path);
    if (!idx) {
        fprintf(stderr, "No index found. Run 'sws index' first.\n");
        return 1;
    }

    sws_result_t *results = calloc(limit, sizeof(sws_result_t));

    double t0 = time_ms();
    int n;
    if (use_fuzzy) {
        n = sws_fuzzy_search(idx, query, results, limit);
    } else {
        n = sws_bm25_search(idx, query, results, limit);
    }
    double elapsed = time_ms() - t0;

    char pathbuf[4096];
    char snippet[512];
    for (int i = 0; i < n; i++) {
        extract_path(results[i].text, pathbuf, sizeof(pathbuf));
        extract_snippet(results[i].text, query, snippet, sizeof(snippet));
        printf("%3d. [%.3f] %s\n", i + 1, results[i].score, pathbuf);
        if (snippet[0]) {
            printf("     ...%.200s...\n", snippet);
        }
    }
    printf("%d result%s (%.1fms)\n", n, n == 1 ? "" : "s", elapsed);

    free(results);
    sws_free(idx);
    return 0;
}

static int cmd_info(fsidx_config_t *cfg) {
    char idx_path[4096], meta_path[4096];
    build_path(idx_path, sizeof(idx_path), cfg->index_dir, "index.sws");
    build_path(meta_path, sizeof(meta_path), cfg->index_dir, "index.meta");

    sws_index_t *idx = sws_load(idx_path);
    if (!idx) {
        fprintf(stderr, "No index found. Run 'sws index' first.\n");
        return 1;
    }

    sws_info_t info;
    sws_info(idx, &info);

    printf("SwarmRT Filesystem Search Index\n");
    printf("  Index:     %s\n", idx_path);
    printf("  Documents: %u\n", info.doc_count);
    printf("  Tokens:    %u\n", info.token_count);
    printf("  Trigrams:  %u\n", info.trigram_count);
    printf("  Memory:    %.1f KB\n", info.memory_bytes / 1024.0);

    /* Show indexed paths from config */
    printf("  Paths:\n");
    for (int i = 0; i < cfg->path_count; i++)
        printf("    - %s\n", cfg->paths[i]);
    if (cfg->path_count == 0)
        printf("    (current directory)\n");

    /* Show index file size */
    struct stat st;
    if (stat(idx_path, &st) == 0) {
        printf("  Index file: %.1f KB\n", st.st_size / 1024.0);
    }
    if (stat(meta_path, &st) == 0) {
        printf("  Meta file:  %.1f KB\n", st.st_size / 1024.0);
    }

    sws_free(idx);
    return 0;
}

static int cmd_watch(fsidx_config_t *cfg, int interval, int force) {
    printf("Watching for changes every %ds (Ctrl+C to stop)\n", interval);

#ifdef _WIN32
    signal(SIGINT, sigint_handler);
#else
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#endif

    /* First run: full index */
    cmd_index(cfg, force, 0, NULL);

    while (g_running) {
        sw_sleep(interval);
        if (!g_running) break;
        printf("\n--- Rescanning ---\n");
        cmd_index(cfg, 0, 0, NULL);
    }

    printf("\nStopped.\n");
    return 0;
}

/* ── Usage ─────────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "SwarmRT Filesystem Search (sws)\n"
        "\n"
        "Usage:\n"
        "  sws index  [--config PATH] [--force] [PATHS...]\n"
        "  sws search [--config PATH] [-n LIMIT] [-t bm25|fuzzy] QUERY\n"
        "  sws info   [--config PATH]\n"
        "  sws watch  [--config PATH] [--interval SECS]\n"
        "\n"
        "Config: ~/.config/sws/config (key = value format)\n"
    );
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];
    const char *config_path = NULL;
    int force = 0;
    int limit = 10;
    int use_fuzzy = 0;
    int watch_interval = 30;

    /* Parse global flags */
    int arg_start = 2;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            arg_start = i + 1;
        } else if (strcmp(argv[i], "--force") == 0) {
            force = 1;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
            arg_start = i + 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "fuzzy") == 0) use_fuzzy = 1;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            watch_interval = atoi(argv[++i]);
            arg_start = i + 1;
        } else {
            break;
        }
    }

    /* Load config */
    fsidx_config_t cfg;
    config_init(&cfg);

    if (config_path) {
        if (config_load(&cfg, config_path) != 0) {
            fprintf(stderr, "Failed to load config: %s\n", config_path);
            config_free(&cfg);
            return 1;
        }
    } else {
        char default_cfg[4096];
        build_path(default_cfg, sizeof(default_cfg), cfg.index_dir, "config");
        config_load(&cfg, default_cfg); /* OK if missing */
    }

    int rc;
    if (strcmp(cmd, "index") == 0) {
        rc = cmd_index(&cfg, force, argc - arg_start, argv + arg_start);
    } else if (strcmp(cmd, "search") == 0) {
        /* Remaining args are the query */
        if (arg_start >= argc) {
            fprintf(stderr, "Usage: sws search QUERY\n");
            config_free(&cfg);
            return 1;
        }
        /* Join remaining args as query */
        char query[4096] = {0};
        for (int i = arg_start; i < argc; i++) {
            if (i > arg_start) strcat(query, " ");
            strncat(query, argv[i], sizeof(query) - strlen(query) - 2);
        }
        rc = cmd_search(&cfg, query, limit, use_fuzzy);
    } else if (strcmp(cmd, "info") == 0) {
        rc = cmd_info(&cfg);
    } else if (strcmp(cmd, "watch") == 0) {
        rc = cmd_watch(&cfg, watch_interval, force);
    } else {
        usage();
        rc = 1;
    }

    config_free(&cfg);
    return rc;
}
