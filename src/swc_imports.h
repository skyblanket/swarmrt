/*
 * swc import resolution — see swc_imports.c.
 *
 * otonomy.ai
 */

#ifndef SWC_IMPORTS_H
#define SWC_IMPORTS_H

#include <limits.h>
#include "swarmrt_lang.h"
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Where `import Foo` is looked for. Shared by `swc build`/`emit`, `swc run`
 * and `swc test` so every path resolves every import identically. */
typedef struct {
    char   input_dir[PATH_MAX];      /* dir of the primary input file */
    char   lib_dir[PATH_MAX];        /* <swarmrt>/lib — bundled stdlib */
    int    has_project;              /* a swarm.json was found upward */
    char   project_root[PATH_MAX];
    char **dep_dirs;                 /* <root>/.swarm/deps/<name>, sorted */
    int    ndeps;
} import_ctx_t;

/* Module name declared by the AST's `module` line. */
const char *get_mod_name(void *ast);

/* dirname() into a caller buffer ("." when there is no separator). */
void path_dirname(const char *path, char *out, size_t outsz);

/* Append `src`'s functions into `dst`: each as "Mod.fn" when `qualify`, and
 * under its bare name when no function of that name exists yet. The main
 * module's own functions must be in dst first, so they win bare-name
 * collisions. AST nodes are shared, not copied. */
void merge_module_funs(node_t *dst, node_t *src, int qualify);

void import_ctx_init(import_ctx_t *c, const char *input_path, const char *lib_dir);
void import_ctx_free(import_ctx_t *c);

/* Load imports transitively: the imports of every module in mods[]
 * (including the ones loaded here), each module once by name, appended to
 * mods/paths (paths added here are strdup'd). With `build`, register each
 * source with codegen for diagnostics and say what was auto-imported.
 * Returns the number of conflicts. */
int load_imports(void **mods, const char **paths, int *nmods, int max,
                 const import_ctx_t *c, int build);

#endif /* SWC_IMPORTS_H */
