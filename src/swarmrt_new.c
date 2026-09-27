/*
 * swc new + locating the swc install (see swarmrt_new.h).
 *
 * A template is a directory of plain files under <install>/templates/<name>.
 * `swc new` copies it into a new project directory, replacing {{name}} with
 * the project name; a file called `gitignore` becomes `.gitignore` (a
 * dotfile inside templates/ would be easy to miss). SWARMRT_HOME overrides
 * the install root for relocated installs.
 *
 * otonomy.ai
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "swarmrt_new.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <direct.h>
  #define sw_mkdir(p) _mkdir(p)
#else
  #include <unistd.h>
  #include <dirent.h>
  #define sw_mkdir(p) mkdir(p, 0755)
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char g_self[PATH_MAX];

const char *sw_swc_self_path(const char *argv0) {
    if (g_self[0]) return g_self;
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);
    if (n > 0) { g_self[n] = '\0'; return g_self; }
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t sz = sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) == 0 && realpath(raw, g_self)) return g_self;
#endif
#ifndef _WIN32
    /* No OS answer: a bare name was found on PATH, so search it the same way. */
    if (argv0 && !strchr(argv0, '/')) {
        const char *path = getenv("PATH");
        char *copy = path ? strdup(path) : NULL;
        for (char *dir = copy ? strtok(copy, ":") : NULL; dir; dir = strtok(NULL, ":")) {
            char cand[PATH_MAX];
            snprintf(cand, sizeof(cand), "%s/%s", *dir ? dir : ".", argv0);
            if (access(cand, X_OK) == 0 && realpath(cand, g_self)) { free(copy); return g_self; }
        }
        free(copy);
    } else if (argv0 && realpath(argv0, g_self)) {
        return g_self;
    }
#endif
    snprintf(g_self, sizeof(g_self), "%s", argv0 ? argv0 : "swc");
    return g_self;
}

/* <install root> = SWARMRT_HOME, else the parent of swc's directory
 * (swc lives in <root>/bin). */
static void install_root(const char *swc_path, char *out, size_t cap) {
    const char *home = getenv("SWARMRT_HOME");
    if (home && *home) { snprintf(out, cap, "%s", home); return; }
    snprintf(out, cap, "%s", swc_path);
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0'; else snprintf(out, cap, ".");
    size_t len = strlen(out);
    snprintf(out + len, cap - len, "/..");
}

static int valid_name(const char *s) {
    if (!s || !(*s >= 'a' && *s <= 'z')) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return strlen(s) <= 64;
}

/* Copy one template file, replacing every {{name}}. */
static int copy_file(const char *src, const char *dst, const char *name) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "swc new: cannot read %s: %s\n", src, strerror(errno)); return -1; }
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t got = buf ? fread(buf, 1, (size_t)len, in) : 0;
    fclose(in);
    if (!buf || got != (size_t)len) { free(buf); fprintf(stderr, "swc new: cannot read %s\n", src); return -1; }
    buf[len] = '\0';
    FILE *out = fopen(dst, "wb");
    if (!out) { free(buf); fprintf(stderr, "swc new: cannot write %s: %s\n", dst, strerror(errno)); return -1; }
    const char *p = buf, *hit;
    while ((hit = strstr(p, "{{name}}"))) {
        fwrite(p, 1, (size_t)(hit - p), out);
        fputs(name, out);
        p = hit + 8;
    }
    fputs(p, out);
    free(buf);
    return fclose(out) == 0 ? 0 : -1;
}

int sw_new_main(int argc, char **argv, const char *swc_path) {
    const char *name = NULL, *tmpl = "agent";
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--template") == 0 && i + 1 < argc) tmpl = argv[++i];
        else if (!name) name = argv[i];
    }
    if (!valid_name(name)) {
        fprintf(stderr, "Usage: swc new <name> [--template agent]\n"
                        "  <name>: lowercase letters, digits, '_' or '-', starting with a letter\n");
        return 1;
    }
    if (!valid_name(tmpl)) { fprintf(stderr, "swc new: bad template name '%s'\n", tmpl); return 1; }

    char root[PATH_MAX], tdir[PATH_MAX];
    install_root(swc_path, root, sizeof(root));
    snprintf(tdir, sizeof(tdir), "%s/templates/%s", root, tmpl);
#ifdef _WIN32
    fprintf(stderr, "swc new: not supported on Windows yet\n");
    return 1;
#else
    DIR *d = opendir(tdir);
    if (!d) { fprintf(stderr, "swc new: no template '%s' (looked in %s)\n", tmpl, tdir); return 1; }
    struct stat st;
    if (stat(name, &st) == 0) {
        closedir(d);
        fprintf(stderr, "swc new: '%s' already exists\n", name);
        return 1;
    }
    if (sw_mkdir(name) != 0) {
        closedir(d);
        fprintf(stderr, "swc new: cannot create %s: %s\n", name, strerror(errno));
        return 1;
    }
    int rc = 0, files = 0;
    struct dirent *e;
    while ((e = readdir(d)) && rc == 0) {
        if (e->d_name[0] == '.') continue;
        char src[PATH_MAX + 256], dst[PATH_MAX + 256];
        snprintf(src, sizeof(src), "%s/%s", tdir, e->d_name);
        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const char *out_name = strcmp(e->d_name, "gitignore") == 0 ? ".gitignore" : e->d_name;
        snprintf(dst, sizeof(dst), "%s/%s", name, out_name);
        rc = copy_file(src, dst, name);
        files++;
    }
    closedir(d);
    if (rc != 0) return 1;
    printf("Created %s/ from the %s template (%d files).\n\n"
           "  cd %s\n"
           "  make test      # offline: a local server plays the model\n"
           "  LLM_PROVIDER=ollama LLM_MODEL=qwen2.5 make run\n",
           name, tmpl, files, name);
    return 0;
#endif
}
