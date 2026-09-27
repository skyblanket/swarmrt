/*
 * swc package manager: `swc add`, `swc install` (alias `swc deps`),
 * `swc update`, `swc remove`.
 *
 *   swarm.json   manifest at the project root — name, version, deps
 *   swarm.lock   pins every git dependency (direct and transitive) to a
 *                resolved commit sha; `swc install` checks out exactly those
 *   .swarm/deps/<name>   one checkout (git) or symlink (path) per dependency
 *
 * Dependencies are flat: one version per name across the whole graph. Two
 * packages asking for the same name with a different source or ref is an
 * error, not a silent pick.
 *
 * git is always run through fork+execvp with an argv vector — never a shell
 * string — and every value that reaches its argv (dependency name, URL, ref,
 * sha) is validated first, so a manifest can't smuggle options or shell
 * syntax into the command. See docs/PACKAGES.md.
 *
 * otonomy.ai
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>
#ifndef _WIN32
  #include <unistd.h>
  #include <dirent.h>
  #include <fcntl.h>
  #include <ftw.h>
  #include <sys/wait.h>
#endif
#include "swarmrt_lang.h"
#include "swc_pkg.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PKG_NAME_MAX 64
#define PKG_URL_MAX  1024
#define PKG_REF_MAX  256

typedef struct {
    char name[PKG_NAME_MAX + 1];
    char git[PKG_URL_MAX];        /* git source ("" for a path dep) */
    char ref[PKG_REF_MAX];        /* requested ref ("" = remote HEAD) */
    char path[PATH_MAX];          /* path source, as written in the manifest */
    char abs_path[PATH_MAX];      /* path source, resolved (install time) */
    char base[PATH_MAX];          /* dir a relative `path` is relative to */
    char sha[65];                 /* resolved / locked commit */
    char via[PKG_NAME_MAX + 1];   /* "" = the project; else the requiring dep */
} pkg_dep_t;

typedef struct {
    pkg_dep_t *v;
    int n, cap;
} pkg_deps_t;

static void deps_push(pkg_deps_t *d, const pkg_dep_t *x) {
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->v = realloc(d->v, sizeof(pkg_dep_t) * (size_t)d->cap);
    }
    d->v[d->n++] = *x;
}

static pkg_dep_t *deps_find(pkg_deps_t *d, const char *name) {
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->v[i].name, name) == 0) return &d->v[i];
    return NULL;
}

static void deps_remove(pkg_deps_t *d, const char *name) {
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->v[i].name, name) == 0) {
            memmove(&d->v[i], &d->v[i + 1], sizeof(pkg_dep_t) * (size_t)(d->n - i - 1));
            d->n--;
            return;
        }
}

/* ---------------------------------------------------------------------
 * Validation — everything that ends up in a git argv goes through here.
 * --------------------------------------------------------------------- */

/* Dependency names: [a-z0-9_-]+, not starting with '-', at most 64 chars.
 * The name is also the directory under .swarm/deps/, so no '/', '.', etc. */
static int valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > PKG_NAME_MAX || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Git URLs: non-empty, not an option ('-...'), no control characters or
 * spaces, and no "<transport>::" remote-helper syntax (ext::, fd::, ...),
 * which can run arbitrary commands. */
static int valid_url(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= PKG_URL_MAX || s[0] == '-') return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] <= ' ' || s[i] == 0x7f) return 0;
    if (strstr(s, "::")) return 0;
    return 1;
}

/* Refs (tag, branch, sha): [A-Za-z0-9._/+-], not starting with '-', no "..". */
static int valid_ref(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= PKG_REF_MAX || s[0] == '-' || strstr(s, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '/' || c == '+' || c == '-'))
            return 0;
    }
    return 1;
}

static int valid_sha(const char *s) {
    size_t n = strlen(s);
    if (n != 40 && n != 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* ---------------------------------------------------------------------
 * Files and paths
 * --------------------------------------------------------------------- */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Write `text` to `path` via a temp file + rename, so a crash never leaves
 * a half-written manifest or lockfile. */
static int write_atomic(const char *path, const char *text) {
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "swc: cannot write %s: %s\n", tmp, strerror(errno)); return -1; }
    fputs(text, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "swc: cannot write %s: %s\n", path, strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

int swc_pkg_find_root(const char *start_dir, char *out, size_t outsz) {
#ifdef _WIN32
    (void)start_dir; (void)out; (void)outsz;
    return 0;   /* packages are not supported on Windows yet */
#else
    char cur[PATH_MAX];
    if (!realpath(start_dir, cur)) return 0;
    for (;;) {
        char probe[PATH_MAX + 32];
        snprintf(probe, sizeof(probe), "%s/%s", cur, SWC_PKG_MANIFEST);
        if (is_file(probe)) { snprintf(out, outsz, "%s", cur); return 1; }
        char *slash = strrchr(cur, '/');
        if (!slash) return 0;
        if (slash == cur) {
            if (cur[1] == '\0') return 0;   /* checked "/" already */
            cur[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
#endif
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int swc_pkg_dep_dirs(const char *root, char ***out) {
    *out = NULL;
#ifdef _WIN32
    (void)root;
    return 0;
#else
    char dir[PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s/%s", root, SWC_PKG_DEPS_DIR);
    DIR *d = opendir(dir);
    if (!d) return 0;
    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!valid_name(e->d_name)) continue;   /* skips ".", "..", stray files */
        char full[PATH_MAX + 320];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (!is_dir(full)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, sizeof(char *) * (size_t)cap); }
        v[n++] = strdup(full);
    }
    closedir(d);
    if (n > 1) qsort(v, (size_t)n, sizeof(char *), cmp_str);
    *out = v;
    return n;
#endif
}

#ifndef _WIN32

/* ---------------------------------------------------------------------
 * Running git — fork + execvp, argv only, never a shell.
 * --------------------------------------------------------------------- */

#define RUN_NOEXEC (-127)   /* the program could not be started at all */

/* Run argv[0] (looked up on PATH) with `argv`. If `out` is non-NULL, the
 * child's stdout is captured there (trailing whitespace trimmed); otherwise
 * it goes to our stderr, so git's progress never pollutes swc's stdout. With
 * `quiet`, the child's stderr is discarded (used for probes that have a
 * fallback). Returns the exit status, or RUN_NOEXEC if exec failed. */
static int run_argv(char *const argv[], char *out, size_t outsz, int quiet) {
    int outp[2] = { -1, -1 }, errp[2];
    if (out && pipe(outp) != 0) return RUN_NOEXEC;
    if (pipe(errp) != 0) return RUN_NOEXEC;
    fcntl(errp[1], F_SETFD, FD_CLOEXEC);
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) return RUN_NOEXEC;
    if (pid == 0) {
        close(errp[0]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) dup2(devnull, 0);
        if (out) { close(outp[0]); dup2(outp[1], 1); close(outp[1]); }
        else dup2(2, 1);
        if (quiet && devnull >= 0) dup2(devnull, 2);
        execvp(argv[0], argv);
        int e = errno;
        ssize_t w = write(errp[1], &e, sizeof(e));
        (void)w;
        _exit(127);
    }
    close(errp[1]);
    size_t len = 0;
    if (out) {
        close(outp[1]);
        char buf[512];
        ssize_t r;
        while ((r = read(outp[0], buf, sizeof(buf))) > 0) {
            size_t take = (size_t)r;
            if (len + take >= outsz) take = outsz - 1 - len;
            memcpy(out + len, buf, take);
            len += take;
        }
        close(outp[0]);
        out[len] = '\0';
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                           out[len - 1] == ' ' || out[len - 1] == '\t'))
            out[--len] = '\0';
    }
    int exec_errno = 0;
    ssize_t r = read(errp[0], &exec_errno, sizeof(exec_errno));
    close(errp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (r == (ssize_t)sizeof(exec_errno)) return RUN_NOEXEC;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* git [-c protocol hardening] -C <dir> <args...>. At most 12 args. */
static int git_in(const char *dir, char *out, size_t outsz, int quiet, ...) {
    char *argv[24];
    int n = 0;
    argv[n++] = "git";
    argv[n++] = "-c"; argv[n++] = "protocol.ext.allow=never";
    argv[n++] = "-c"; argv[n++] = "protocol.fd.allow=never";
    argv[n++] = "-c"; argv[n++] = "advice.detachedHead=false";
    if (dir) { argv[n++] = "-C"; argv[n++] = (char *)dir; }
    va_list ap;
    va_start(ap, quiet);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && n < 23) argv[n++] = (char *)a;
    va_end(ap);
    argv[n] = NULL;
    return run_argv(argv, out, outsz, quiet);
}

static int g_git_checked = 0;

static int require_git(void) {
    if (g_git_checked) return 0;
    char ver[128];
    int rc = git_in(NULL, ver, sizeof(ver), 1, "--version", (char *)NULL);
    if (rc != 0) {
        fprintf(stderr, "swc: git not found in PATH — it is needed to fetch git dependencies "
                        "(install git, or use {\"path\": ...} dependencies)\n");
        return -1;
    }
    g_git_checked = 1;
    return 0;
}

/* Recursive delete for .swarm/deps entries. nftw with FTW_PHYS never follows
 * symlinks, so removing a path dependency's link never touches its target. */
static int rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(p);
}

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    return nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

#endif /* !_WIN32 */

/* ---------------------------------------------------------------------
 * JSON output
 * --------------------------------------------------------------------- */

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void indent(FILE *f, int n) { for (int i = 0; i < n; i++) fputs("  ", f); }

/* Generic value writer — used to carry manifest fields swc doesn't manage
 * (description, license, ...) through `swc add` / `swc remove` unchanged. */
static void json_val(FILE *f, sw_val_t *v, int depth) {
    if (!v) { fputs("null", f); return; }
    switch (v->type) {
        case SW_VAL_NIL:    fputs("null", f); break;
        case SW_VAL_INT:    fprintf(f, "%lld", (long long)v->v.i); break;
        case SW_VAL_FLOAT:  fprintf(f, "%.17g", v->v.f); break;
        case SW_VAL_STRING: json_str(f, v->v.str); break;
        case SW_VAL_ATOM:
            if (strcmp(v->v.str, "true") == 0 || strcmp(v->v.str, "false") == 0) fputs(v->v.str, f);
            else json_str(f, v->v.str);
            break;
        case SW_VAL_LIST:
            fputc('[', f);
            for (int i = 0; i < v->v.tuple.count; i++) {
                if (i) fputs(", ", f);
                json_val(f, v->v.tuple.items[i], depth + 1);
            }
            fputc(']', f);
            break;
        case SW_VAL_MAP:
            if (v->v.map.count == 0) { fputs("{}", f); break; }
            fputs("{\n", f);
            for (int i = 0; i < v->v.map.count; i++) {
                indent(f, depth + 1);
                sw_val_t *k = v->v.map.keys[i];
                json_str(f, (k && (k->type == SW_VAL_ATOM || k->type == SW_VAL_STRING)) ? k->v.str : "");
                fputs(": ", f);
                json_val(f, v->v.map.vals[i], depth + 1);
                fputs(i + 1 < v->v.map.count ? ",\n" : "\n", f);
            }
            indent(f, depth);
            fputc('}', f);
            break;
        default: fputs("null", f); break;
    }
}

/* ---------------------------------------------------------------------
 * Manifest (swarm.json) and lockfile (swarm.lock)
 * --------------------------------------------------------------------- */

typedef struct {
    char path[PATH_MAX];      /* the swarm.json file */
    char dir[PATH_MAX];       /* its directory (the package / project root) */
    sw_val_t *doc;            /* the decoded document (for unmanaged fields) */
    char name[128];
    char version[64];
    pkg_deps_t deps;
} pkg_manifest_t;

static const char *map_str(sw_val_t *m, const char *key) {
    sw_val_t *v = sw_val_map_get(m, sw_val_atom(key));
    return (v && v->type == SW_VAL_STRING) ? v->v.str : NULL;
}

/* Parse one dependency spec {"git": url, "ref": r} | {"path": dir} (plus
 * "sha"/"via" in the lockfile). Returns 0 or prints an error and returns -1. */
static int parse_dep(const char *file, const char *name, sw_val_t *spec, int is_lock, pkg_dep_t *d) {
    memset(d, 0, sizeof(*d));
    if (!valid_name(name)) {
        fprintf(stderr, "swc: %s: invalid dependency name '%s' (use [a-z0-9_-], at most %d chars)\n",
                file, name, PKG_NAME_MAX);
        return -1;
    }
    snprintf(d->name, sizeof(d->name), "%s", name);
    if (!spec || spec->type != SW_VAL_MAP) {
        fprintf(stderr, "swc: %s: dependency '%s' must be an object like "
                        "{\"git\": \"<url>\", \"ref\": \"<tag>\"} or {\"path\": \"<dir>\"}\n", file, name);
        return -1;
    }
    const char *git = map_str(spec, "git");
    const char *path = map_str(spec, "path");
    const char *ref = map_str(spec, "ref");
    if ((git != NULL) == (path != NULL)) {
        fprintf(stderr, "swc: %s: dependency '%s' needs exactly one of \"git\" or \"path\"\n", file, name);
        return -1;
    }
    if (git) {
        if (!valid_url(git)) {
            fprintf(stderr, "swc: %s: dependency '%s' has an invalid git URL '%s'\n", file, name, git);
            return -1;
        }
        snprintf(d->git, sizeof(d->git), "%s", git);
        if (ref) {
            if (!valid_ref(ref)) {
                fprintf(stderr, "swc: %s: dependency '%s' has an invalid ref '%s'\n", file, name, ref);
                return -1;
            }
            snprintf(d->ref, sizeof(d->ref), "%s", ref);
        }
    } else {
        if (!path[0]) {
            fprintf(stderr, "swc: %s: dependency '%s' has an empty path\n", file, name);
            return -1;
        }
        snprintf(d->path, sizeof(d->path), "%s", path);
    }
    if (is_lock) {
        const char *sha = map_str(spec, "sha");
        const char *via = map_str(spec, "via");
        if (git && (!sha || !valid_sha(sha))) {
            fprintf(stderr, "swc: %s: entry '%s' has no valid \"sha\"\n", file, name);
            return -1;
        }
        if (sha) snprintf(d->sha, sizeof(d->sha), "%s", sha);
        if (via && valid_name(via)) snprintf(d->via, sizeof(d->via), "%s", via);
    }
    return 0;
}

static int parse_deps_map(const char *file, sw_val_t *deps, int is_lock, pkg_deps_t *out) {
    if (!deps || deps->type == SW_VAL_NIL) return 0;
    if (deps->type != SW_VAL_MAP) {
        fprintf(stderr, "swc: %s: \"deps\" must be an object\n", file);
        return -1;
    }
    for (int i = 0; i < deps->v.map.count; i++) {
        sw_val_t *k = deps->v.map.keys[i];
        const char *name = (k && (k->type == SW_VAL_ATOM || k->type == SW_VAL_STRING)) ? k->v.str : "";
        pkg_dep_t d;
        if (parse_dep(file, name, deps->v.map.vals[i], is_lock, &d) != 0) return -1;
        if (deps_find(out, d.name)) {
            fprintf(stderr, "swc: %s: dependency '%s' is listed twice\n", file, d.name);
            return -1;
        }
        deps_push(out, &d);
    }
    return 0;
}

/* Load <dir>/swarm.json. Returns 0, or -1 after printing why. */
static int manifest_load(const char *dir, pkg_manifest_t *m) {
    memset(m, 0, sizeof(*m));
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
    snprintf(m->path, sizeof(m->path), "%s/%s", dir, SWC_PKG_MANIFEST);
    char *text = slurp(m->path);
    if (!text) { fprintf(stderr, "swc: cannot read %s\n", m->path); return -1; }
    sw_val_t *doc = sw_lang_json_decode(text);
    free(text);
    if (!doc || doc->type != SW_VAL_MAP) {
        fprintf(stderr, "swc: %s is not a valid JSON object\n", m->path);
        return -1;
    }
    m->doc = doc;
    const char *name = map_str(doc, "name");
    const char *ver = map_str(doc, "version");
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
    snprintf(m->version, sizeof(m->version), "%s", ver ? ver : "");
    sw_val_t *deps = sw_val_map_get(doc, sw_val_atom("deps"));
    return parse_deps_map(m->path, deps, 0, &m->deps);
}

static void dep_spec_json(FILE *f, const pkg_dep_t *d, int lock) {
    fputc('{', f);
    if (d->git[0]) {
        fputs("\"git\": ", f); json_str(f, d->git);
        if (d->ref[0]) { fputs(", \"ref\": ", f); json_str(f, d->ref); }
        if (lock) { fputs(", \"sha\": ", f); json_str(f, d->sha); }
    } else {
        fputs("\"path\": ", f); json_str(f, d->path);
    }
    if (lock && d->via[0]) { fputs(", \"via\": ", f); json_str(f, d->via); }
    fputc('}', f);
}

/* Rewrite swarm.json: name, version, every unmanaged field in its original
 * order, then deps. */
static char *manifest_render(const pkg_manifest_t *m) {
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    fputs("{\n  \"name\": ", f); json_str(f, m->name);
    fputs(",\n  \"version\": ", f); json_str(f, m->version[0] ? m->version : "0.1.0");
    if (m->doc && m->doc->type == SW_VAL_MAP) {
        for (int i = 0; i < m->doc->v.map.count; i++) {
            sw_val_t *k = m->doc->v.map.keys[i];
            if (!k || (k->type != SW_VAL_ATOM && k->type != SW_VAL_STRING)) continue;
            if (!strcmp(k->v.str, "name") || !strcmp(k->v.str, "version") || !strcmp(k->v.str, "deps"))
                continue;
            fputs(",\n  ", f); json_str(f, k->v.str); fputs(": ", f);
            json_val(f, m->doc->v.map.vals[i], 1);
        }
    }
    fputs(",\n  \"deps\": {", f);
    for (int i = 0; i < m->deps.n; i++) {
        fputs(i ? ",\n    " : "\n    ", f);
        json_str(f, m->deps.v[i].name); fputs(": ", f);
        dep_spec_json(f, &m->deps.v[i], 0);
    }
    fputs(m->deps.n ? "\n  }\n}\n" : "}\n}\n", f);
    fclose(f);
    return buf;
}

static int cmp_dep(const void *a, const void *b) {
    return strcmp(((const pkg_dep_t *)a)->name, ((const pkg_dep_t *)b)->name);
}

static char *lock_render(pkg_deps_t *resolved) {
    pkg_dep_t *sorted = malloc(sizeof(pkg_dep_t) * (size_t)(resolved->n ? resolved->n : 1));
    if (resolved->n) memcpy(sorted, resolved->v, sizeof(pkg_dep_t) * (size_t)resolved->n);
    qsort(sorted, (size_t)resolved->n, sizeof(pkg_dep_t), cmp_dep);
    char *buf = NULL; size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) { free(sorted); return NULL; }
    fputs("{\n  \"lock_version\": 1,\n  \"deps\": {", f);
    for (int i = 0; i < resolved->n; i++) {
        fputs(i ? ",\n    " : "\n    ", f);
        json_str(f, sorted[i].name); fputs(": ", f);
        dep_spec_json(f, &sorted[i], 1);
    }
    fputs(resolved->n ? "\n  }\n}\n" : "}\n}\n", f);
    fclose(f);
    free(sorted);
    return buf;
}

/* Load swarm.lock if present. Returns 0 (possibly with no entries), or -1 if
 * the file exists but is unusable. */
static int lock_load(const char *root, pkg_deps_t *out) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", root, SWC_PKG_LOCKFILE);
    char *text = slurp(path);
    if (!text) return 0;
    sw_val_t *doc = sw_lang_json_decode(text);
    free(text);
    if (!doc || doc->type != SW_VAL_MAP) {
        fprintf(stderr, "swc: %s is not valid JSON — delete it to re-resolve every dependency\n", path);
        return -1;
    }
    if (parse_deps_map(path, sw_val_map_get(doc, sw_val_atom("deps")), 1, out) != 0) {
        fprintf(stderr, "swc: fix %s or delete it to re-resolve every dependency\n", path);
        return -1;
    }
    return 0;
}

#ifndef _WIN32

/* ---------------------------------------------------------------------
 * Fetching
 * --------------------------------------------------------------------- */

static int have_commit(const char *dir, const char *sha) {
    char spec[96];
    snprintf(spec, sizeof(spec), "%s^{commit}", sha);
    return git_in(dir, NULL, 0, 1, "cat-file", "-e", spec, (char *)NULL) == 0;
}

/* Fetch every branch and tag (unshallowing if needed; a tag that moved on
 * the remote is updated, not refused). Loud: on failure the user sees git's
 * own reason, then ours. */
static int fetch_all(const char *dir, const pkg_dep_t *d) {
    char shallow[PATH_MAX + 32];
    snprintf(shallow, sizeof(shallow), "%s/.git/shallow", dir);
    int rc;
    if (is_file(shallow))
        rc = git_in(dir, NULL, 0, 0, "fetch", "-q", "--unshallow", "origin",
                    "+refs/heads/*:refs/remotes/origin/*", "+refs/tags/*:refs/tags/*", (char *)NULL);
    else
        rc = git_in(dir, NULL, 0, 0, "fetch", "-q", "origin",
                    "+refs/heads/*:refs/remotes/origin/*", "+refs/tags/*:refs/tags/*", (char *)NULL);
    if (rc != 0) {
        fprintf(stderr, "swc: could not fetch '%s' from %s — network failure, no access, "
                        "or no such repository (git exited %d)\n", d->name, d->git, rc);
        return -1;
    }
    return 0;
}

/* Make `dir` (.swarm/deps/<name>) a git checkout of d->git at d->sha (the
 * locked commit when `locked` is set, else the commit d->ref resolves to,
 * which is written into d->sha). Sets *created when it made `dir`. */
static int fetch_git_into(const char *dir, pkg_dep_t *d, int locked, int *created) {

    /* Reuse an existing checkout only if it is a git repo of the same URL;
     * anything else there (a path-dep symlink, a stale clone) is replaced. */
    struct stat st;
    if (lstat(dir, &st) == 0) {
        char gitdir[PATH_MAX + 160], origin[PKG_URL_MAX];
        snprintf(gitdir, sizeof(gitdir), "%s/.git", dir);
        int reuse = S_ISDIR(st.st_mode) && is_dir(gitdir) &&
                    git_in(dir, origin, sizeof(origin), 1, "remote", "get-url", "origin", (char *)NULL) == 0 &&
                    strcmp(origin, d->git) == 0;
        if (!reuse && rm_rf(dir) != 0) {
            fprintf(stderr, "swc: cannot remove stale %s: %s\n", dir, strerror(errno));
            return -1;
        }
    }
    if (!is_dir(dir)) {
        *created = 1;
        if (mkdir_p(dir) != 0) {
            fprintf(stderr, "swc: cannot create %s: %s\n", dir, strerror(errno));
            return -1;
        }
        if (git_in(dir, NULL, 0, 1, "init", "-q", (char *)NULL) != 0 ||
            git_in(dir, NULL, 0, 0, "remote", "add", "origin", d->git, (char *)NULL) != 0) {
            fprintf(stderr, "swc: could not initialise a git checkout in %s\n", dir);
            return -1;
        }
    }

    if (locked) {
        /* Exactly the locked commit: shallow fetch by sha where the server
         * allows it, else fetch everything and look for it. */
        if (!have_commit(dir, d->sha)) {
            git_in(dir, NULL, 0, 1, "fetch", "-q", "--depth", "1", "origin", d->sha, (char *)NULL);
            if (!have_commit(dir, d->sha)) {
                if (fetch_all(dir, d) != 0) return -1;
                if (!have_commit(dir, d->sha)) {
                    fprintf(stderr, "swc: locked commit %s of '%s' is not in %s any more "
                                    "(history rewritten?) — run `swc update` to re-resolve\n",
                            d->sha, d->name, d->git);
                    return -1;
                }
            }
        }
    } else {
        const char *ref = d->ref[0] ? d->ref : "HEAD";
        char sha[128] = "";
        if (git_in(dir, NULL, 0, 1, "fetch", "-q", "--depth", "1", "origin", ref, (char *)NULL) == 0 &&
            git_in(dir, sha, sizeof(sha), 1, "rev-parse", "--verify", "-q", "FETCH_HEAD^{commit}",
                   (char *)NULL) == 0 && valid_sha(sha)) {
            /* resolved by the shallow fetch */
        } else {
            /* Not a branch/tag the server hands out by name (e.g. an
             * abbreviated sha), or a network problem: fetch everything,
             * which reports the network problem loudly. */
            if (fetch_all(dir, d) != 0) return -1;
            const char *forms[3] = { "%s^{commit}", "refs/remotes/origin/%s^{commit}", "refs/tags/%s^{commit}" };
            sha[0] = '\0';
            for (int i = 0; i < 3 && !sha[0]; i++) {
                char spec[PKG_REF_MAX + 64];
                snprintf(spec, sizeof(spec), forms[i], ref);
                if (git_in(dir, sha, sizeof(sha), 1, "rev-parse", "--verify", "-q", spec, (char *)NULL) != 0 ||
                    !valid_sha(sha))
                    sha[0] = '\0';
            }
            if (!sha[0]) {
                fprintf(stderr, "swc: bad ref '%s' for '%s': no such tag, branch or commit in %s\n",
                        ref, d->name, d->git);
                return -1;
            }
        }
        snprintf(d->sha, sizeof(d->sha), "%s", sha);
    }

    if (git_in(dir, NULL, 0, 0, "checkout", "-q", "--force", "--detach", d->sha, (char *)NULL) != 0) {
        fprintf(stderr, "swc: could not check out %s of '%s'\n", d->sha, d->name);
        return -1;
    }
    char head[128] = "";
    if (git_in(dir, head, sizeof(head), 1, "rev-parse", "HEAD", (char *)NULL) != 0 ||
        strcmp(head, d->sha) != 0) {
        fprintf(stderr, "swc: sha mismatch for '%s': expected %s, checked out %s\n",
                d->name, d->sha, head[0] ? head : "(nothing)");
        return -1;
    }
    return 0;
}

static int fetch_git(const char *root, pkg_dep_t *d, int locked) {
    if (require_git() != 0) return -1;
    char dir[PATH_MAX + 128];
    snprintf(dir, sizeof(dir), "%s/%s/%s", root, SWC_PKG_DEPS_DIR, d->name);
    int created = 0;
    int rc = fetch_git_into(dir, d, locked, &created);
    if (rc != 0 && created) rm_rf(dir);   /* don't leave a half-made checkout */
    return rc;
}

/* Point .swarm/deps/<name> at a local directory (symlink — edits to the
 * local package are picked up without reinstalling). */
static int link_path(const char *root, pkg_dep_t *d) {
    char src[PATH_MAX * 2 + 2];
    if (d->path[0] == '/') snprintf(src, sizeof(src), "%s", d->path);
    else snprintf(src, sizeof(src), "%s/%s", d->base, d->path);
    if (!realpath(src, d->abs_path) || !is_dir(d->abs_path)) {
        fprintf(stderr, "swc: path dependency '%s': %s is not a directory\n", d->name, src);
        return -1;
    }
    char link[PATH_MAX + 128];
    snprintf(link, sizeof(link), "%s/%s/%s", root, SWC_PKG_DEPS_DIR, d->name);
    struct stat st;
    if (lstat(link, &st) == 0) {
        char cur[PATH_MAX];
        ssize_t n = S_ISLNK(st.st_mode) ? readlink(link, cur, sizeof(cur) - 1) : -1;
        if (n > 0) { cur[n] = '\0'; if (strcmp(cur, d->abs_path) == 0) return 0; }
        if (rm_rf(link) != 0) {
            fprintf(stderr, "swc: cannot remove stale %s: %s\n", link, strerror(errno));
            return -1;
        }
    }
    if (symlink(d->abs_path, link) != 0) {
        fprintf(stderr, "swc: cannot link %s -> %s: %s\n", link, d->abs_path, strerror(errno));
        return -1;
    }
    return 0;
}

static int same_source(const pkg_dep_t *a, const pkg_dep_t *b) {
    if (a->git[0] || b->git[0])
        return strcmp(a->git, b->git) == 0 && strcmp(a->ref, b->ref) == 0;
    return strcmp(a->abs_path, b->abs_path) == 0;
}

static void describe(const pkg_dep_t *d, char *out, size_t n) {
    if (d->git[0]) snprintf(out, n, "%s@%s", d->git, d->ref[0] ? d->ref : "HEAD");
    else snprintf(out, n, "path %s", d->abs_path[0] ? d->abs_path : d->path);
}

/* Does <lib>/<file> exist (as given, or with the first letter's case
 * flipped — the resolver tries both Foo.sw and foo.sw)? */
static int lib_has(const char *lib_dir, const char *file) {
    char p[PATH_MAX * 2];
    snprintf(p, sizeof(p), "%s/%s", lib_dir, file);
    if (is_file(p)) return 1;
    char alt[PATH_MAX];
    snprintf(alt, sizeof(alt), "%s", file);
    if (alt[0] >= 'a' && alt[0] <= 'z') alt[0] -= 32;
    else if (alt[0] >= 'A' && alt[0] <= 'Z') alt[0] += 32;
    snprintf(p, sizeof(p), "%s/%s", lib_dir, alt);
    return is_file(p);
}

/* Warn when a dependency's module would shadow a bundled lib/ module. */
static void warn_lib_collisions(const char *root, pkg_deps_t *resolved, const char *lib_dir) {
    if (!lib_dir || !is_dir(lib_dir)) return;
    for (int i = 0; i < resolved->n; i++) {
        const char *subs[2] = { "", "/src" };
        for (int s = 0; s < 2; s++) {
            char dir[PATH_MAX + 160];
            snprintf(dir, sizeof(dir), "%s/%s/%s%s", root, SWC_PKG_DEPS_DIR, resolved->v[i].name, subs[s]);
            DIR *dd = opendir(dir);
            if (!dd) continue;
            struct dirent *e;
            while ((e = readdir(dd)) != NULL) {
                size_t n = strlen(e->d_name);
                if (n < 4 || strcmp(e->d_name + n - 3, ".sw") != 0) continue;
                if (lib_has(lib_dir, e->d_name))
                    fprintf(stderr, "swc: warning: dependency '%s' provides %s, which shadows the "
                                    "bundled lib/ module of that name (the dependency wins)\n",
                            resolved->v[i].name, e->d_name);
            }
            closedir(dd);
        }
    }
}

/* Remove .swarm/deps entries that are no longer part of the graph. */
static void prune(const char *root, pkg_deps_t *resolved) {
    char dir[PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s/%s", root, SWC_PKG_DEPS_DIR);
    DIR *d = opendir(dir);
    if (!d) return;
    char **gone = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (deps_find(resolved, e->d_name)) continue;
        gone = realloc(gone, sizeof(char *) * (size_t)(n + 1));
        gone[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[PATH_MAX + 320];
        snprintf(p, sizeof(p), "%s/%s", dir, gone[i]);
        if (rm_rf(p) == 0) fprintf(stderr, "swc: removed %s\n", gone[i]);
        free(gone[i]);
    }
    free(gone);
}

/* Install the whole dependency graph of the project at `root` into
 * .swarm/deps and (re)write swarm.lock. With `use_lock`, git deps whose
 * source and ref match their lock entry are checked out at the locked sha;
 * everything else is resolved fresh. */
static int pkg_sync(const pkg_manifest_t *proj, int use_lock, const char *lib_dir) {
    const char *root = proj->dir;
    pkg_deps_t lock = {0}, queue = {0}, resolved = {0};
    int rc = -1;
    if (use_lock && lock_load(root, &lock) != 0) goto out;

    char deps_dir[PATH_MAX + 32];
    snprintf(deps_dir, sizeof(deps_dir), "%s/%s", root, SWC_PKG_DEPS_DIR);
    if (mkdir_p(deps_dir) != 0) {
        fprintf(stderr, "swc: cannot create %s: %s\n", deps_dir, strerror(errno));
        goto out;
    }

    for (int i = 0; i < proj->deps.n; i++) {
        pkg_dep_t d = proj->deps.v[i];
        snprintf(d.base, sizeof(d.base), "%s", root);
        d.via[0] = '\0';
        deps_push(&queue, &d);
    }

    /* Breadth-first over the graph: the project's own deps first, so a
     * conflict is reported against the project's choice. */
    for (int qi = 0; qi < queue.n; qi++) {
        pkg_dep_t d = queue.v[qi];
        if (d.path[0] && !d.git[0]) {
            char src[PATH_MAX * 2 + 2];
            if (d.path[0] == '/') snprintf(src, sizeof(src), "%s", d.path);
            else snprintf(src, sizeof(src), "%s/%s", d.base, d.path);
            if (!realpath(src, d.abs_path)) d.abs_path[0] = '\0';
        }
        pkg_dep_t *have = deps_find(&resolved, d.name);
        if (have) {
            if (same_source(have, &d)) continue;
            char a[PKG_URL_MAX + PATH_MAX + 8], b[PKG_URL_MAX + PATH_MAX + 8];
            describe(have, a, sizeof(a));
            describe(&d, b, sizeof(b));
            fprintf(stderr, "swc: dependency conflict for '%s': %s wants %s, but %s wants %s "
                            "(dependencies are flat — one version per name)\n",
                    d.name, have->via[0] ? have->via : "the project", a,
                    d.via[0] ? d.via : "the project", b);
            goto out;
        }
        if (d.git[0]) {
            pkg_dep_t *l = deps_find(&lock, d.name);
            int locked = l && strcmp(l->git, d.git) == 0 && strcmp(l->ref, d.ref) == 0 && l->sha[0];
            if (locked) snprintf(d.sha, sizeof(d.sha), "%s", l->sha);
            if (fetch_git(root, &d, locked) != 0) goto out;
            fprintf(stderr, "swc: %-16s %.12s  %s@%s%s\n", d.name, d.sha, d.git,
                    d.ref[0] ? d.ref : "HEAD", locked ? " (locked)" : "");
        } else {
            if (link_path(root, &d) != 0) goto out;
            fprintf(stderr, "swc: %-16s path  %s\n", d.name, d.abs_path);
        }
        deps_push(&resolved, &d);

        /* Transitive: the dep's own manifest, paths relative to the dep. */
        char pkg_dir[PATH_MAX + 128];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", deps_dir, d.name);
        char sub_manifest[PATH_MAX + 160];
        snprintf(sub_manifest, sizeof(sub_manifest), "%s/%s", pkg_dir, SWC_PKG_MANIFEST);
        if (is_file(sub_manifest)) {
            pkg_manifest_t sub;
            if (manifest_load(pkg_dir, &sub) != 0) {
                fprintf(stderr, "swc: (in dependency '%s')\n", d.name);
                goto out;
            }
            char base[PATH_MAX];
            if (!realpath(pkg_dir, base)) {
                fprintf(stderr, "swc: cannot resolve %s: %s\n", pkg_dir, strerror(errno));
                free(sub.deps.v);
                goto out;
            }
            for (int j = 0; j < sub.deps.n; j++) {
                pkg_dep_t t = sub.deps.v[j];
                snprintf(t.base, sizeof(t.base), "%s", base);
                snprintf(t.via, sizeof(t.via), "%s", d.name);
                deps_push(&queue, &t);
            }
            free(sub.deps.v);
        }
    }

    prune(root, &resolved);

    char *text = lock_render(&resolved);
    char lock_path[PATH_MAX + 32];
    snprintf(lock_path, sizeof(lock_path), "%s/%s", root, SWC_PKG_LOCKFILE);
    if (!text || write_atomic(lock_path, text) != 0) { free(text); goto out; }
    free(text);

    warn_lib_collisions(root, &resolved, lib_dir);
    fprintf(stderr, "swc: %d dependenc%s installed in %s/, locked in %s\n",
            resolved.n, resolved.n == 1 ? "y" : "ies", SWC_PKG_DEPS_DIR, SWC_PKG_LOCKFILE);
    rc = 0;
out:
    free(lock.v); free(queue.v); free(resolved.v);
    return rc;
}

/* ---------------------------------------------------------------------
 * Commands
 * --------------------------------------------------------------------- */

static int load_project(pkg_manifest_t *m) {
    char root[PATH_MAX];
    if (!swc_pkg_find_root(".", root, sizeof(root))) {
        fprintf(stderr, "swc: no %s found in this directory or any parent.\n"
                        "swc: create one at your project root, e.g.\n"
                        "       {\"name\": \"myapp\", \"version\": \"0.1.0\", \"deps\": {}}\n",
                SWC_PKG_MANIFEST);
        return -1;
    }
    return manifest_load(root, m);
}

/* Write the manifest, run the install, and put the old manifest back if the
 * install fails — a failed `swc add` leaves swarm.json as it was. */
static int commit_manifest(pkg_manifest_t *m, const char *lib_dir) {
    char *old = slurp(m->path);
    char *text = manifest_render(m);
    if (!text || write_atomic(m->path, text) != 0) { free(text); free(old); return 1; }
    free(text);
    if (pkg_sync(m, 1, lib_dir) != 0) {
        if (old) write_atomic(m->path, old);
        fprintf(stderr, "swc: %s left unchanged\n", SWC_PKG_MANIFEST);
        free(old);
        return 1;
    }
    free(old);
    return 0;
}

static void add_usage(void) {
    fprintf(stderr, "usage: swc add <name> <git-url>[@ref]\n"
                    "       swc add <name> --path <dir>\n");
}

static int cmd_add(int argc, char **argv, const char *lib_dir) {
    if (argc < 4) { add_usage(); return 1; }
    const char *name = argv[2];
    if (!valid_name(name)) {
        fprintf(stderr, "swc: invalid dependency name '%s' (use [a-z0-9_-], at most %d chars, "
                        "not starting with '-')\n", name, PKG_NAME_MAX);
        return 1;
    }
    pkg_dep_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.name, sizeof(d.name), "%s", name);
    if (strcmp(argv[3], "--path") == 0) {
        if (argc != 5) { add_usage(); return 1; }
        snprintf(d.path, sizeof(d.path), "%s", argv[4]);
    } else {
        if (argc != 4) { add_usage(); return 1; }
        const char *src = argv[3];
        if (strlen(src) >= PKG_URL_MAX) { fprintf(stderr, "swc: URL too long\n"); return 1; }
        snprintf(d.git, sizeof(d.git), "%s", src);
        /* url@ref: the last '@' after the last '/' and ':' (so the user@
         * of git@host:org/repo is never taken for a ref). */
        char *at = strrchr(d.git, '@');
        char *slash = strrchr(d.git, '/');
        char *colon = strrchr(d.git, ':');
        if (at && (!slash || at > slash) && (!colon || at > colon)) {
            *at = '\0';
            snprintf(d.ref, sizeof(d.ref), "%s", at + 1);
            if (!valid_ref(d.ref)) {
                fprintf(stderr, "swc: invalid ref '%s'\n", d.ref);
                return 1;
            }
        }
        if (!valid_url(d.git)) {
            fprintf(stderr, "swc: refusing git URL '%s' (must not start with '-', contain "
                            "whitespace, or use a '<transport>::' helper)\n", d.git);
            return 1;
        }
    }

    pkg_manifest_t m;
    if (load_project(&m) != 0) return 1;
    if (d.path[0] && d.path[0] != '/') {
        /* A relative --path is relative to where the user typed it; the
         * manifest stores it relative to the project root when run there,
         * else as an absolute path. */
        char cwd[PATH_MAX];
        if (!realpath(".", cwd) || strcmp(cwd, m.dir) != 0) {
            char abs[PATH_MAX];
            if (!realpath(d.path, abs)) {
                fprintf(stderr, "swc: path dependency '%s': %s is not a directory\n", name, d.path);
                return 1;
            }
            snprintf(d.path, sizeof(d.path), "%s", abs);
        }
    }
    pkg_dep_t *have = deps_find(&m.deps, name);
    if (have) *have = d; else deps_push(&m.deps, &d);
    int rc = commit_manifest(&m, lib_dir);
    if (rc == 0) fprintf(stderr, "swc: added %s\n", name);
    return rc;
}

static int cmd_remove(int argc, char **argv, const char *lib_dir) {
    if (argc != 3) { fprintf(stderr, "usage: swc remove <name>\n"); return 1; }
    const char *name = argv[2];
    if (!valid_name(name)) { fprintf(stderr, "swc: invalid dependency name '%s'\n", name); return 1; }
    pkg_manifest_t m;
    if (load_project(&m) != 0) return 1;
    if (!deps_find(&m.deps, name)) {
        fprintf(stderr, "swc: '%s' is not a dependency in %s\n", name, m.path);
        return 1;
    }
    deps_remove(&m.deps, name);
    int rc = commit_manifest(&m, lib_dir);
    if (rc == 0) fprintf(stderr, "swc: removed %s from %s\n", name, SWC_PKG_MANIFEST);
    return rc;
}

static int cmd_install(int argc, char **argv, const char *lib_dir, int use_lock) {
    if (argc != 2) {
        fprintf(stderr, "usage: swc %s\n", argv[1]);
        return 1;
    }
    pkg_manifest_t m;
    if (load_project(&m) != 0) return 1;
    return pkg_sync(&m, use_lock, lib_dir) == 0 ? 0 : 1;
}

#endif /* !_WIN32 */

int swc_pkg_is_command(const char *cmd) {
    return !strcmp(cmd, "add") || !strcmp(cmd, "install") || !strcmp(cmd, "deps") ||
           !strcmp(cmd, "update") || !strcmp(cmd, "remove");
}

int swc_pkg_main(int argc, char **argv, const char *lib_dir) {
#ifdef _WIN32
    (void)argc; (void)lib_dir;
    fprintf(stderr, "swc: `swc %s` is not supported on Windows yet\n", argv[1]);
    return 1;
#else
    /* git must never block on a credential prompt; a private repo without
     * credentials is a clear failure, not a hang. */
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    const char *cmd = argv[1];
    if (!strcmp(cmd, "add"))    return cmd_add(argc, argv, lib_dir);
    if (!strcmp(cmd, "remove")) return cmd_remove(argc, argv, lib_dir);
    if (!strcmp(cmd, "update")) return cmd_install(argc, argv, lib_dir, 0);
    return cmd_install(argc, argv, lib_dir, 1);   /* install / deps */
#endif
}
