/*
 * swc import resolution — where `import Foo` is looked for, and the
 * transitive load of every imported module. Shared by `swc run` and
 * `swc test` so both resolve imports identically (siblings, then the
 * installed packages in swarm.lock, then the bundled lib/).
 *
 * otonomy.ai
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swarmrt_lang.h"
#include "swarmrt_codegen.h"
#include "swc_imports.h"
#include "swc_pkg.h"
#include "swarmrt_readfile.h"
#include <limits.h>
#ifndef _WIN32
  #include <libgen.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Silent read — returns NULL without a message. Used when probing import
 * candidate paths (a miss is expected, not an error). */
static char *read_file_quiet(const char *path) {
    return sw_read_file_all(path);
}

/* Extract module name from AST */
const char *get_mod_name(void *ast) {
    node_t *mod = (node_t *)ast;
    return mod->v.mod.name;
}

/* dirname() into a caller buffer ("." when there is no separator). */
void path_dirname(const char *path, char *out, size_t outsz) {
    char *tmp = strdup(path);
    if (!tmp) { snprintf(out, outsz, "."); return; }
#ifdef _WIN32
    char *sep = strrchr(tmp, '/');
    char *bsep = strrchr(tmp, '\\');
    if (bsep > sep) sep = bsep;
    if (sep) *sep = '\0'; else tmp[0] = '.', tmp[1] = '\0';
    snprintf(out, outsz, "%s", tmp);
#else
    snprintf(out, outsz, "%s", dirname(tmp));
#endif
    free(tmp);
}

void import_ctx_init(import_ctx_t *c, const char *input_path, const char *lib_dir) {
    memset(c, 0, sizeof(*c));
    path_dirname(input_path, c->input_dir, sizeof(c->input_dir));
    snprintf(c->lib_dir, sizeof(c->lib_dir), "%s", lib_dir);
    c->has_project = swc_pkg_find_root(c->input_dir, c->project_root, sizeof(c->project_root));
    if (c->has_project)
        c->ndeps = swc_pkg_dep_dirs(c->project_root, &c->dep_dirs);
}

void import_ctx_free(import_ctx_t *c) {
    for (int i = 0; i < c->ndeps; i++) free(c->dep_dirs[i]);
    free(c->dep_dirs);
    c->dep_dirs = NULL; c->ndeps = 0;
}

/* Probe <dir>/<Name>.sw then <dir>/<name>.sw, silently (a miss on one
 * candidate is expected; only a total miss is reported). */
static char *probe_module(const char *dir, const char *name, const char *lower,
                          char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s.sw", dir, name);
    char *src = read_file_quiet(out);
    if (src) return src;
    snprintf(out, outsz, "%s/%s.sw", dir, lower);
    return read_file_quiet(out);
}

/* A dependency package's modules live in its root or its src/. */
static char *probe_package(const char *pkg, const char *name, const char *lower,
                           char *out, size_t outsz) {
    char *src = probe_module(pkg, name, lower, out, outsz);
    if (src) return src;
    char sub[PATH_MAX + 8];
    snprintf(sub, sizeof(sub), "%s/src", pkg);
    return probe_module(sub, name, lower, out, outsz);
}

/* Find and read the source of `import <name>` written in the module at
 * `importer`. Lookup order:
 *   1. the importer's own package (root, then src/) when the importer is a
 *      dependency's module; otherwise the primary input file's directory
 *   2. every installed dependency: <project>/.swarm/deps/<pkg>/ and its src/
 *      (a module provided by two packages is an error, not a silent pick)
 *   3. <swarmrt>/lib/  (bundled stdlib)
 * Each directory is tried as <Name>.sw, then <name>.sw. Returns the malloc'd
 * source (its path in `out`), or NULL after printing why. */
static char *resolve_import(const import_ctx_t *c, const char *importer, const char *name,
                            char *out, size_t outsz) {
    char lower[128];
    snprintf(lower, sizeof(lower), "%s", name);
    for (int i = 0; lower[i]; i++)
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 32;

    int own = -1;
    for (int i = 0; i < c->ndeps && importer; i++) {
        size_t n = strlen(c->dep_dirs[i]);
        if (strncmp(importer, c->dep_dirs[i], n) == 0 && importer[n] == '/') { own = i; break; }
    }
    char *src = own >= 0 ? probe_package(c->dep_dirs[own], name, lower, out, outsz)
                         : probe_module(c->input_dir, name, lower, out, outsz);
    if (src) return src;

    for (int i = 0; i < c->ndeps; i++) {
        if (i == own) continue;
        char path[PATH_MAX + 256];
        char *s = probe_package(c->dep_dirs[i], name, lower, path, sizeof(path));
        if (!s) continue;
        if (src) {
            fprintf(stderr, "swc: import '%s' is ambiguous: both %s and %s provide it\n",
                    name, out, path);
            free(s); free(src);
            return NULL;
        }
        src = s;
        snprintf(out, outsz, "%s", path);
    }
    if (src) return src;

    src = probe_module(c->lib_dir, name, lower, out, outsz);
    if (src) return src;
    if (c->has_project)
        fprintf(stderr, "swc: cannot resolve import '%s' (looked in %s/, %s/%s/*/ and %s/)"
                        " — if it comes from a dependency, run `swc install`\n",
                name, own >= 0 ? c->dep_dirs[own] : c->input_dir,
                c->project_root, SWC_PKG_DEPS_DIR, c->lib_dir);
    else
        fprintf(stderr, "swc: cannot resolve import '%s' (looked in %s/ and %s/)\n",
                name, c->input_dir, c->lib_dir);
    return NULL;
}

/* Is `importer` a module inside an installed dependency? */
static int importer_in_dep(const import_ctx_t *c, const char *importer) {
    if (!importer) return 0;
    for (int i = 0; i < c->ndeps; i++) {
        size_t n = strlen(c->dep_dirs[i]);
        if (strncmp(importer, c->dep_dirs[i], n) == 0 && importer[n] == '/') return 1;
    }
    return 0;
}

static int same_file(const char *a, const char *b) {
    char ra[PATH_MAX], rb[PATH_MAX];
    if (realpath(a, ra) && realpath(b, rb)) return strcmp(ra, rb) == 0;
    return strcmp(a, b) == 0;
}

/* Load imports transitively: the imports of every module in mods[]
 * (including the ones loaded here), each module once by name, appended to
 * mods/paths (paths added here are strdup'd). With `build`, register each
 * source with codegen for diagnostics and say what was auto-imported.
 * Returns the number of conflicts: module names are global, so a package
 * whose own import resolves to a different file than the module already
 * loaded under that name can't be built (it would silently call the other
 * module otherwise). */
int load_imports(void **mods, const char **paths, int *nmods, int max,
                        const import_ctx_t *c, int build) {
    int conflicts = 0;
    for (int a = 0; a < *nmods; a++) {
        node_t *m = (node_t *)mods[a];
        for (int im = 0; im < m->v.mod.nimports; im++) {
            const char *imp_name = m->v.mod.imports[im];
            int loaded = -1;
            for (int e = 0; e < *nmods; e++)
                if (strcmp(get_mod_name(mods[e]), imp_name) == 0) { loaded = e; break; }
            if (loaded >= 0) {
                /* A package's own imports resolve inside the package first:
                 * check that the name really is the same file. */
                if (importer_in_dep(c, paths[a]) && paths[loaded]) {
                    char want[PATH_MAX + 256];
                    char *s = resolve_import(c, paths[a], imp_name, want, sizeof(want));
                    if (s) {
                        free(s);
                        if (!same_file(want, paths[loaded])) {
                            fprintf(stderr, "swc: module '%s' is provided by both %s and %s "
                                            "(imported from %s); module names are global, "
                                            "so rename one\n",
                                    imp_name, paths[loaded], want, paths[a]);
                            conflicts++;
                        }
                    }
                }
                continue;
            }

            char imp_path[PATH_MAX + 256];
            char *imp_source = resolve_import(c, paths[a], imp_name, imp_path, sizeof(imp_path));
            if (!imp_source) continue;
            /* The file found is already loaded (its module line names some
             * other module): don't load it again for every importer. */
            int dup = 0;
            for (int e = 0; e < *nmods; e++)
                if (paths[e] && strcmp(paths[e], imp_path) == 0) { dup = 1; break; }
            if (dup) { free(imp_source); continue; }
            void *imp_ast = sw_lang_parse(imp_source);
            free(imp_source);
            if (!imp_ast) {
                fprintf(stderr, "swc: parse failed for import '%s'\n", imp_name);
                continue;
            }
            if (*nmods < max) {
                paths[*nmods] = strdup(imp_path);
                mods[(*nmods)++] = imp_ast;
                if (build) {
                    sw_codegen_register_source(get_mod_name(imp_ast), imp_path);
                    fprintf(stderr, "swc: auto-imported %s from %s\n", imp_name, imp_path);
                }
            }
        }
    }
    return conflicts;
}

/* Prepend the globals of mods[1..nmods-1] to dst's globals, keeping load
 * order among the imports. Built back to front so the result reads
 * imports first, dst last. */
void merge_module_globals(node_t *dst, void **mods, int nmods) {
    for (int a = nmods - 1; a >= 1; a--) {
        node_t *src = (node_t *)mods[a];
        int n = src->v.mod.nglobals, old = dst->v.mod.nglobals;
        if (n == 0) continue;
        dst->v.mod.globals = realloc(dst->v.mod.globals,
                                     sizeof(*dst->v.mod.globals) * (size_t)(old + n));
        memmove(&dst->v.mod.globals[n], dst->v.mod.globals,
                sizeof(*dst->v.mod.globals) * (size_t)old);
        memcpy(dst->v.mod.globals, src->v.mod.globals,
               sizeof(*src->v.mod.globals) * (size_t)n);
        dst->v.mod.nglobals = old + n;
    }
}

/* Append `src`'s functions into `dst`. Each imported function is added
 * under its module-qualified name "Mod.fn" (so `Mod.fn(...)` call sites
 * resolve in the interpreter, which stores qualified calls as a single
 * "Mod.fn" identifier) AND, if no function of that bare name already
 * exists in dst, under its bare name too (so an imported function's body
 * — which calls its module-siblings unqualified — keeps working). The
 * main module's own functions are added first, so they win bare-name
 * collisions. AST nodes are shared, not copied; the dst module owns the
 * merged funs[] array. */
void merge_module_funs(node_t *dst, node_t *src, int qualify) {
    for (int i = 0; i < src->v.mod.nfuns; i++) {
        node_t *fn = src->v.mod.funs[i];
        const char *bare = fn->v.fun.name;

        if (qualify) {
            /* Qualified copy: a shallow node clone sharing the same body,
             * but named "Mod.bare". Shallow is safe — the body/params are
             * read-only during evaluation. */
            node_t *q = (node_t *)malloc(sizeof(node_t));
            *q = *fn;
            snprintf(q->v.fun.name, sizeof(q->v.fun.name), "%s.%s",
                     src->v.mod.name, bare);
            dst->v.mod.nfuns++;
            dst->v.mod.funs = realloc(dst->v.mod.funs,
                                      sizeof(node_t *) * dst->v.mod.nfuns);
            dst->v.mod.funs[dst->v.mod.nfuns - 1] = q;
        }

        /* Bare name — only if not already present (first-wins). */
        int exists = 0;
        for (int j = 0; j < dst->v.mod.nfuns; j++)
            if (strcmp(dst->v.mod.funs[j]->v.fun.name, bare) == 0) { exists = 1; break; }
        if (!exists) {
            dst->v.mod.nfuns++;
            dst->v.mod.funs = realloc(dst->v.mod.funs,
                                      sizeof(node_t *) * dst->v.mod.nfuns);
            dst->v.mod.funs[dst->v.mod.nfuns - 1] = fn;
        }
    }
}
