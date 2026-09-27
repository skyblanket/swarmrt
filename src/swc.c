/*
 * SwarmRT Compiler CLI: swc
 *
 * Usage:
 *   swc build <file.sw> [lib.sw ...] [-o name] [-O] [--obfusc] [--strip] [--emit-c]
 *   swc emit  <file.sw> [lib.sw ...]
 *
 * Pipeline: .sw → parse → AST → codegen → .c → cc → binary
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
#include <sys/stat.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #define swc_unlink(p) _unlink(p)
#else
  #include <unistd.h>
  #include <libgen.h>
  #define swc_unlink(p) unlink(p)
#endif
#include "swarmrt_platform.h"
#include "swarmrt_lang.h"
#include "swarmrt_codegen.h"
#include "swarmrt_repl.h"
#include "swarmrt_test.h"
#include "swarmrt_new.h"
#include "swarmrt_native.h"
#include "swarmrt_io.h"
#include "swc_pkg.h"
#include <pthread.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void usage(void) {
    fprintf(stderr,
        "Usage: swc <command> [options] <file.sw>\n\n"
        "Commands:\n"
        "  build    Compile .sw to native binary\n"
        "  run      Interpret a .sw file (calls main())\n"
        "  emit     Output generated C to stdout\n"
        "  repl     Start interactive REPL\n"
        "  test     Run test_* functions in .sw files\n"
        "  new      Create a project: swc new <name> [--template agent]\n"
        "  add      Add a dependency: add <name> <git-url>[@ref] | add <name> --path <dir>\n"
        "  install  Install the deps in swarm.json at the swarm.lock commits (alias: deps)\n"
        "  update   Re-resolve every dependency's ref and rewrite swarm.lock\n"
        "  remove   Remove a dependency: remove <name>\n"
        "  version  Print the swc/runtime version (also --version, -v)\n\n"
        "Options:\n"
        "  -o <name>          Output binary name (default: module name)\n"
        "  -O                 Optimize (-O2)\n"
        "  --obfusc           Enable obfuscation (XOR strings + symbol mangle)\n"
        "  --strip            Strip symbols from binary\n"
        "  --emit-c           Save generated .c file (don't delete after compile)\n"
        "  --target=<triple>  Cross-compile (e.g. x86_64-linux-gnu, aarch64-apple-darwin)\n"
        "                     Needs `zig` or a matching cross-gcc for non-darwin targets.\n");
}

/* Read entire file into malloc'd string */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "swc: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Silent read — returns NULL without a message. Used when probing import
 * candidate paths (a miss is expected, not an error). */
static char *read_file_quiet(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Extract module name from AST */
static const char *get_mod_name(void *ast) {
    node_t *mod = (node_t *)ast;
    return mod->v.mod.name;
}

/* Extract function names from AST */
static int get_func_names(void *ast, const char **names, int max) {
    node_t *mod = (node_t *)ast;
    int n = 0;
    for (int i = 0; i < mod->v.mod.nfuns && n < max; i++)
        names[n++] = mod->v.mod.funs[i]->v.fun.name;
    return n;
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
static void merge_module_funs(node_t *dst, node_t *src, int qualify) {
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

/* dirname() into a caller buffer ("." when there is no separator). */
static void path_dirname(const char *path, char *out, size_t outsz) {
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

/* Where `import Foo` is looked for. Shared by `swc build`/`emit` and
 * `swc run` so both paths resolve every import identically. */
typedef struct {
    char   input_dir[PATH_MAX];      /* dir of the primary input file */
    char   lib_dir[PATH_MAX];        /* <swarmrt>/lib — bundled stdlib */
    int    has_project;              /* a swarm.json was found upward */
    char   project_root[PATH_MAX];
    char **dep_dirs;                 /* <root>/.swarm/deps/<name>, sorted */
    int    ndeps;
} import_ctx_t;

static void import_ctx_init(import_ctx_t *c, const char *input_path, const char *lib_dir) {
    memset(c, 0, sizeof(*c));
    path_dirname(input_path, c->input_dir, sizeof(c->input_dir));
    snprintf(c->lib_dir, sizeof(c->lib_dir), "%s", lib_dir);
    c->has_project = swc_pkg_find_root(c->input_dir, c->project_root, sizeof(c->project_root));
    if (c->has_project)
        c->ndeps = swc_pkg_dep_dirs(c->project_root, &c->dep_dirs);
}

static void import_ctx_free(import_ctx_t *c) {
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

/* Load imports transitively: the imports of every module in mods[]
 * (including the ones loaded here), each module once by name, appended to
 * mods/paths (paths added here are strdup'd). With `build`, register each
 * source with codegen for diagnostics and say what was auto-imported. */
static void load_imports(void **mods, const char **paths, int *nmods, int max,
                         const import_ctx_t *c, int build) {
    for (int a = 0; a < *nmods; a++) {
        node_t *m = (node_t *)mods[a];
        for (int im = 0; im < m->v.mod.nimports; im++) {
            const char *imp_name = m->v.mod.imports[im];
            int loaded = 0;
            for (int e = 0; e < *nmods; e++)
                if (strcmp(get_mod_name(mods[e]), imp_name) == 0) { loaded = 1; break; }
            if (loaded) continue;

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
}

/* Bootstrap context handed to the spawned root process. Mirrors the
 * compiled entrypoint's _main_entry/_sw_done_* machinery (codegen
 * emit_entry_and_main): the root process calls main() inside a live
 * scheduler (so tls_current is set → self/send/receive/spawn/monitor/link
 * behave exactly as compiled), records the exit code, then signals the
 * waiting OS thread to tear the runtime down. */
typedef struct {
    sw_interp_t       *interp;
    int                rc;
    pthread_mutex_t    lock;
    pthread_cond_t     cond;
    volatile int       done;
} run_boot_ctx_t;

/* Root process entry. Runs main() under a real process context, then
 * signals the OS thread (identical shape to codegen's _main_entry). */
static void run_main_entry(void *arg) {
    run_boot_ctx_t *ctx = (run_boot_ctx_t *)arg;
    sw_lang_call(ctx->interp, "main", NULL, 0);
    ctx->rc = ctx->interp->error ? 1 : 0;
    if (ctx->interp->error)
        fprintf(stderr, "swc: runtime error: %s\n", ctx->interp->error_msg);
    pthread_mutex_lock(&ctx->lock);
    ctx->done = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

/* `swc run <file.sw>` — interpret a .sw program: parse the file, resolve
 * its `import` declarations from the local dir and the bundled lib/, merge
 * all functions into one module, build an interpreter, and call main().
 * The documented interpreter run path (SW_LANGUAGE.md). Returns the
 * process exit code. */
static int run_file(const char *path, const char *argv0, int argc, char **argv) {
    char *source = read_file(path);
    if (!source) return 1;
    void *main_ast = sw_lang_parse(source);
    free(source);
    if (!main_ast) { fprintf(stderr, "swc: parse failed for %s\n", path); return 1; }
    node_t *root = (node_t *)main_ast;

    /* swc-binary dir → <root>/lib fallback for stdlib imports. */
    char swarmrt_lib[512] = "./lib";
    {
        char swc_dir[256];
        path_dirname(argv0, swc_dir, sizeof(swc_dir));
        snprintf(swarmrt_lib, sizeof(swarmrt_lib), "%s/../lib", swc_dir);
    }

    /* Load imports the way the build path does: transitively (an imported
     * module's own imports too), same lookup order, each module once. */
    void *mods[64];
    const char *mod_paths[64];
    int nmods = 0;
    mods[nmods] = root; mod_paths[nmods++] = strdup(path);
    {
        import_ctx_t ictx;
        import_ctx_init(&ictx, path, swarmrt_lib);
        load_imports(mods, mod_paths, &nmods, 64, &ictx, 0);
        import_ctx_free(&ictx);
    }

    /* Same static name check as `swc build` — each module against the whole
     * set, before merging — so both paths reject the same programs. */
    {
        int unresolved = 0;
        for (int a = 0; a < nmods; a++)
            unresolved += sw_resolve_module(mods[a], mods, nmods, mod_paths[a]);
        for (int a = 0; a < nmods; a++) free((char *)mod_paths[a]);
        if (unresolved) {
            fprintf(stderr, "swc: %d name error%s — not running\n",
                    unresolved, unresolved == 1 ? "" : "s");
            return 1;
        }
    }

    /* Merge every imported module's functions into the root module. */
    for (int a = 1; a < nmods; a++)
        merge_module_funs(root, (node_t *)mods[a], 1);

    /* Run main(). */
    sw_interp_t *interp = sw_lang_new(main_ast);
    int has_main = 0;
    for (int i = 0; i < root->v.mod.nfuns; i++)
        if (strcmp(root->v.mod.funs[i]->v.fun.name, "main") == 0) { has_main = 1; break; }
    if (!has_main) {
        fprintf(stderr, "swc: no main() in %s\n", path);
        sw_lang_free(interp);
        return 1;
    }
    /* Run main() inside a real scheduler/root-process, replicating the
     * compiled entrypoint (codegen emit_entry_and_main) verbatim so that
     * self()/send/receive/spawn/monitor/link see a live process context
     * (tls_current). Before this, main() ran on the bare OS main thread
     * with tls_current==NULL, diverging from compiled on every
     * concurrency primitive. */
    sw_prog_argc = argc;
    sw_prog_argv = argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Interpreter run path: ALWAYS single-scheduler, regardless of
     * SW_SCHEDULERS. Every sw process (root + every spawned child) is a
     * cooperative fiber multiplexed onto ONE OS thread, so the shared
     * sw_interp_t (interp->error/call_depth/panicking/try_depth) and the
     * shared global_env hash buckets are never touched concurrently. The
     * tree-walking interpreter has no per-process interp isolation, so a
     * second scheduler thread would put a child's eval() on another OS
     * thread and race those fields → UB. Hence spawning real child fibers
     * (N_SPAWN) is only safe under nsched==1.
     *
     * KNOWN/ACCEPTED TRADEOFF: single-scheduler `swc run` can deadlock on
     * in-process HTTP self-loopback (a Phase-3 limitation) — acceptable for
     * the dev/test interpreter; data races are strictly worse. The COMPILED
     * path keeps its own nproc scheduler count (codegen) — do NOT change it. */
    int nsched = 1;

    run_boot_ctx_t boot;
    boot.interp = interp;
    boot.rc = 0;
    boot.done = 0;
    pthread_mutex_init(&boot.lock, NULL);
    pthread_cond_init(&boot.cond, NULL);

    /* The interpreter tree-walks on the C stack (no TCO), so give the root
     * process the deep stack it used to have running on the OS main thread.
     * Lazy mmap → shallow programs touch only a few pages. Set before sw_init
     * so the root process slot is allocated at this size. */
    sw_proc_stack_size = 8 * 1024 * 1024;

    sw_init(get_mod_name(main_ast), (uint32_t)nsched);
    sw_io_init();
    sw_install_shutdown_signals();   /* SIGTERM/SIGINT → graceful drain (Phase 4) */
    sw_spawn(run_main_entry, &boot);

    /* Wait for the root process's main() to return (set by run_main_entry) OR
     * a SIGTERM/SIGINT to request shutdown, whichever first. An abnormal root
     * exit (panic / non-zero reason) is handled inline by the scheduler's
     * root_exit_check via exit(1) — matching compiled — so we never reach here
     * in that case. */
    int normal = sw_wait_for_exit(&boot.done, &boot.lock, &boot.cond);
    int rc = boot.rc;

    sw_io_shutdown();
    if (normal) { sw_shutdown(0); }
    else { sw_shutdown_graceful(0, -1); }   /* signal → drain then teardown */
    pthread_cond_destroy(&boot.cond);
    pthread_mutex_destroy(&boot.lock);

    /* Don't node_free the merged module: its funs[] mixes shared imported
     * nodes with shallow qualified clones (sharing bodies), so a recursive
     * free would double-free. Same trade-off the test runner makes — let
     * process exit reclaim the AST. */
    interp->module_ast = NULL;
    sw_lang_free(interp);
    return rc;
}

#ifndef SWARMRT_VERSION
#define SWARMRT_VERSION "0.0.0-dev"   /* Makefile injects the real value from ./VERSION */
#endif

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    /* Every install-relative lookup below starts from argv[0]; make it the
     * real path even when swc was found on PATH. */
    argv[0] = (char *)sw_swc_self_path(argv[0]);
    sw_interp_batteries_install();   /* battery builtins for run/test/REPL */

    const char *cmd = argv[1];

    /* Version — before anything else so it never needs a swarm/init. Prints the
     * semver from ./VERSION; when built from a git checkout also the git
     * describe (e.g. "1.0.0-rc.1 (1.0.0-rc.1-3-gabc1234)") so a dev build is
     * distinguishable from a tagged release. */
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-v") == 0) {
#ifdef SWARMRT_GITDESC
        printf("swc %s (%s)\n", SWARMRT_VERSION, SWARMRT_GITDESC);
#else
        printf("swc %s\n", SWARMRT_VERSION);
#endif
        return 0;
    }

    /* REPL — no input files needed */
    if (strcmp(cmd, "new") == 0)
        return sw_new_main(argc, argv, argv[0]);

    if (strcmp(cmd, "repl") == 0)
        return sw_repl_start();

    /* LSP server — speaks LSP/JSON-RPC over stdin/stdout. */
    if (strcmp(cmd, "lsp") == 0) {
        extern int sw_lsp_main(void);
        return sw_lsp_main();
    }

    /* Test runner */
    if (strcmp(cmd, "test") == 0) {
        if (argc < 3) {
            /* Default: run tests in current directory */
            return sw_test_run_dir(".") ? 1 : 0;
        }
        int total_failures = 0;
        for (int i = 2; i < argc; i++) {
            /* Check if argument is a directory */
            struct stat st;
            if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode))
                total_failures += sw_test_run_dir(argv[i]);
            else
                total_failures += sw_test_run_file(argv[i]);
        }
        return total_failures ? 1 : 0;
    }

    /* Interpreter run path — `swc run <file.sw>`. Documented in
     * SW_LANGUAGE.md; previously printed "unknown command 'run'". */
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { fprintf(stderr, "swc: run needs a .sw file\n"); return 1; }
        return run_file(argv[2], argv[0], argc, argv);
    }

    /* Package manager — swarm.json / swarm.lock / .swarm/deps (swc_pkg.c). */
    if (swc_pkg_is_command(cmd)) {
        char swc_dir[256], lib_dir[512];
        path_dirname(argv[0], swc_dir, sizeof(swc_dir));
        snprintf(lib_dir, sizeof(lib_dir), "%s/../lib", swc_dir);
        return swc_pkg_main(argc, argv, lib_dir);
    }

    if (argc < 3) { usage(); return 1; }
    const char *inputs[64];
    int ninputs = 0;
    const char *output_name = NULL;
    const char *target = NULL;       /* --target=<triple> for cross-compile */
    int optimize = 0, obfusc = 0, strip = 0, emit_c = 0;

    /* Parse arguments */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_name = argv[++i];
        else if (strcmp(argv[i], "-O") == 0)
            optimize = 1;
        else if (strcmp(argv[i], "--obfusc") == 0)
            obfusc = 1;
        else if (strcmp(argv[i], "--strip") == 0)
            strip = 1;
        else if (strcmp(argv[i], "--emit-c") == 0)
            emit_c = 1;
        else if (strncmp(argv[i], "--target=", 9) == 0)
            target = argv[i] + 9;
        else if (argv[i][0] != '-' && ninputs < 64)
            inputs[ninputs++] = argv[i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "swc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (ninputs == 0) { fprintf(stderr, "swc: no input file\n"); return 1; }
    const char *input = inputs[0]; /* primary input */

    /* Parse all input files */
    void *asts[64];
    const char *ast_paths[64];
    int nasts = 0;
    int main_idx = 0;

    for (int i = 0; i < ninputs; i++) {
        char *source = read_file(inputs[i]);
        if (!source) return 1;
        void *ast = sw_lang_parse(source);
        free(source);
        if (!ast) { fprintf(stderr, "swc: parse failed for %s\n", inputs[i]); return 1; }
        asts[nasts] = ast;
        ast_paths[nasts] = inputs[i];
        /* Diagnostics + #line directives point at the REAL file. */
        sw_codegen_register_source(get_mod_name(ast), inputs[i]);

        /* Find which module has main() */
        const char *fnames[64];
        int nf = get_func_names(ast, fnames, 64);
        for (int j = 0; j < nf; j++)
            if (strcmp(fnames[j], "main") == 0) main_idx = nasts;
        nasts++;
    }

    /* Compute the swc-binary directory and the swarmrt root so the
     * import resolver can fall back to <root>/lib/ for stdlib modules
     * (Std, Time, Cron, etc.) without the user having to copy them. */
    char swc_dir_early[256] = ".";
    char swarmrt_lib[512] = "./lib";
    {
        char *sp = strdup(argv[0]);
        if (sp) {
#ifdef _WIN32
            char *sep = strrchr(sp, '/');
            char *bsep = strrchr(sp, '\\');
            if (bsep > sep) sep = bsep;
            if (sep) *sep = '\0'; else sp[0] = '.', sp[1] = '\0';
            snprintf(swc_dir_early, sizeof(swc_dir_early), "%s", sp);
#else
            char *dir = dirname(sp);
            snprintf(swc_dir_early, sizeof(swc_dir_early), "%s", dir);
#endif
            free(sp);
        }
        snprintf(swarmrt_lib, sizeof(swarmrt_lib), "%s/../lib", swc_dir_early);
    }

    /* Resolve imports transitively for every parsed AST — see
     * resolve_import for the lookup order (input dir, installed deps, lib/). */
    {
        import_ctx_t ictx;
        import_ctx_init(&ictx, inputs[0], swarmrt_lib);
        load_imports(asts, ast_paths, &nasts, 64, &ictx, 1);
        import_ctx_free(&ictx);
    }

    const char *mod_name = get_mod_name(asts[main_idx]);

    /* Reject undefined variables before generating any code. */
    {
        int unresolved = 0;
        for (int a = 0; a < nasts; a++)
            unresolved += sw_resolve_module(asts[a], asts, nasts, ast_paths[a]);
        if (unresolved) {
            fprintf(stderr, "swc: %d name error%s — not compiling\n",
                    unresolved, unresolved == 1 ? "" : "s");
            return 1;
        }
    }

    /* ---- emit command ---- */
    if (strcmp(cmd, "emit") == 0) {
        char *code = NULL;
        if (nasts > 1) {
            /* Multi-module: combine all ASTs */
            char *buf = NULL; size_t blen = 0;
            FILE *mf = open_memstream(&buf, &blen);
            if (!mf || sw_codegen_multi(asts, nasts, main_idx, mf) != 0) {
                fprintf(stderr, "swc: codegen failed\n"); return 1;
            }
            fclose(mf);
            code = buf;
        } else {
            code = sw_codegen_to_string(asts[main_idx], 0);
        }
        if (!code) { fprintf(stderr, "swc: codegen failed\n"); return 1; }
        if (obfusc) {
            const char *fnames[64];
            int nfuncs = get_func_names(asts[main_idx], fnames, 64);
            char *obf = sw_obfuscate(code, mod_name, fnames, nfuncs);
            free(code);
            code = obf;
        }
        printf("%s", code);
        free(code);
        return 0;
    }

    /* ---- build command ---- */
    if (strcmp(cmd, "build") != 0) {
        fprintf(stderr, "swc: unknown command '%s'\n", cmd);
        return 1;
    }

    /* Generate C code */
    char *code = NULL;
    if (nasts > 1) {
        char *buf = NULL; size_t blen = 0;
        FILE *mf = open_memstream(&buf, &blen);
        if (!mf || sw_codegen_multi(asts, nasts, main_idx, mf) != 0) {
            fprintf(stderr, "swc: codegen failed\n"); return 1;
        }
        fclose(mf);
        code = buf;
    } else {
        code = sw_codegen_to_string(asts[main_idx], 0);
    }
    if (!code) { fprintf(stderr, "swc: codegen failed\n"); return 1; }

    /* Apply obfuscation if requested */
    if (obfusc) {
        const char *fnames[64];
        int nfuncs = get_func_names(asts[main_idx], fnames, 64);
        char *obf = sw_obfuscate(code, mod_name, fnames, nfuncs);
        free(code);
        code = obf;
    }

    /* Write to temp file */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "%s/swc_%s_XXXXXX.c", sw_tmpdir(), mod_name);
#ifdef _WIN32
    int fd = -1;  /* No mkstemps on Windows, use fallback */
#else
    int fd = mkstemps(tmppath, 2);
#endif
    if (fd < 0) {
        /* Fallback: use fixed name */
        snprintf(tmppath, sizeof(tmppath), "%s/swc_%s.c", sw_tmpdir(), mod_name);
        FILE *tf = fopen(tmppath, "w");
        if (!tf) { fprintf(stderr, "swc: cannot create temp file\n"); free(code); return 1; }
        fputs(code, tf);
        fclose(tf);
    } else {
        FILE *tf = fdopen(fd, "w");
        fputs(code, tf);
        fclose(tf);
    }
    free(code);

    /* Also save .c if requested */
    if (emit_c) {
        char cpath[256];
        char *base = strdup(input);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(cpath, sizeof(cpath), "%s.gen.c", base);
        free(base);

        FILE *cf = fopen(cpath, "w");
        if (cf) {
            char *saved = read_file(tmppath);
            if (saved) { fputs(saved, cf); free(saved); }
            fclose(cf);
            fprintf(stderr, "swc: saved %s\n", cpath);
        }
    }

    /* Determine output name */
    char out_path[256];
    if (output_name) {
        strncpy(out_path, output_name, sizeof(out_path) - 1);
    } else {
        /* Use lowercase module name */
        char lower[128];
        strncpy(lower, mod_name, sizeof(lower) - 1);
        for (int i = 0; lower[i]; i++)
            if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 32;
        snprintf(out_path, sizeof(out_path), "%s", lower);
    }

    /* Find swc's own directory for -I and -L paths */
    char swc_dir[256] = ".";
    char *swc_path = strdup(argv[0]);
    if (swc_path) {
#ifdef _WIN32
        char *sep = strrchr(swc_path, '/');
        char *bsep = strrchr(swc_path, '\\');
        if (bsep > sep) sep = bsep;
        if (sep) *sep = '\0'; else swc_path[0] = '.', swc_path[1] = '\0';
        snprintf(swc_dir, sizeof(swc_dir), "%s/..", swc_path);
#else
        char *dir = dirname(swc_path);
        snprintf(swc_dir, sizeof(swc_dir), "%s/..", dir);
#endif
        free(swc_path);
    }

    /* Compile with cc */
    char cmd_buf[2048];
#ifdef __APPLE__
    /* macOS links libm via libSystem implicitly; -lm is harmless.
     * -lz: the PDF engine (swarmrt_pdf.o, linked when a program imports
     * Pdf) inflates FlateDecode streams; zlib ships with macOS. Without it
     * every program that called pdf_* failed to link here.
     * When the host was built with -DSWARMRT_TLS, generated binaries that
     * call wsc_connect_tls(wss://) also need OpenSSL linked. We pass the
     * Homebrew openssl@3 prefix that the Makefile used. */
  #ifdef SWARMRT_TLS
    const char *extra_libs =
        "-lsqlite3 -lz -lm "
        "-L" SWARMRT_OPENSSL_PREFIX "/lib -lssl -lcrypto "
        "-I" SWARMRT_OPENSSL_PREFIX "/include";
  #else
    const char *extra_libs = "-lsqlite3 -lz -lm";
  #endif
#else
    /* Linux glibc requires explicit -lm — generated C uses fmod() and
     * friends, and ld --as-needed rejects implicit libm dependencies. */
    const char *extra_libs = "-lssl -lcrypto -lsqlite3 -lz -lm";
#endif

    /* --target=<triple> support. Recipes:
     *   <arch>-apple-darwin  → `cc -arch <arch>` (native cross between
     *                           macOS arches works with the same toolchain)
     *   <arch>-linux-gnu     → prefer `zig cc -target <triple>`, fall back
     *                           to `<triple>-cc` (a real cross-gcc).
     * Other triples just pass straight to `zig cc -target` if zig exists.
     *
     * NOTE: libswarmrt.a is built for the HOST arch. Cross-compiling
     * needs a target-arch libswarmrt.a too. This wiring is the host
     * side; document the target-arch build path in BUILDING.md. */
    char cc_cmd[256] = "cc";
    char arch_flags[128] = "";
    if (target) {
        if (strstr(target, "-apple-darwin")) {
            char arch[64] = {0};
            const char *dash = strchr(target, '-');
            int alen = dash ? (int)(dash - target) : (int)strlen(target);
            if (alen > 63) alen = 63;
            memcpy(arch, target, alen);
            snprintf(arch_flags, sizeof(arch_flags), "-arch %s", arch);
            /* cc with -arch handles cross-macOS-arch out of the box. */
        } else if (strstr(target, "-linux") || strstr(target, "-musl") ||
                   strstr(target, "-windows") || strstr(target, "-wasi")) {
            /* Prefer zig cc — it ships a self-contained cross toolchain. */
            if (system("command -v zig >/dev/null 2>&1") == 0) {
                snprintf(cc_cmd, sizeof(cc_cmd), "zig cc -target %s", target);
            } else {
                /* Try <triple>-cc as a real gcc cross-toolchain. */
                char probe[256];
                snprintf(probe, sizeof(probe), "command -v %s-cc >/dev/null 2>&1", target);
                if (system(probe) == 0) {
                    snprintf(cc_cmd, sizeof(cc_cmd), "%s-cc", target);
                } else {
                    fprintf(stderr,
                        "swc: --target=%s requires either `zig` (recommended:\n"
                        "     brew install zig) or `%s-cc` in PATH.\n",
                        target, target);
                    return 1;
                }
            }
        } else {
            /* Unknown triple — pass through to zig cc, error if no zig. */
            if (system("command -v zig >/dev/null 2>&1") != 0) {
                fprintf(stderr, "swc: --target=%s requires `zig` in PATH\n", target);
                return 1;
            }
            snprintf(cc_cmd, sizeof(cc_cmd), "zig cc -target %s", target);
        }
        fprintf(stderr, "swc: cross-compiling for %s via `%s`\n", target, cc_cmd);
    }

    /* If the host was built with TLS, the generated C must see -DSWARMRT_TLS
     * too (the studio header guards wss handshake code on this macro), and on
     * non-Apple platforms OpenSSL is unconditionally available. */
#if defined(SWARMRT_TLS) || !defined(__APPLE__)
    const char *tls_define = "-DSWARMRT_TLS";
#else
    const char *tls_define = "";
#endif

    snprintf(cmd_buf, sizeof(cmd_buf),
        "%s %s %s %s -fno-stack-protector -o %s %s -I%s/src -L%s/bin -lswarmrt -pthread %s %s",
        cc_cmd, arch_flags, optimize ? "-O2" : "-O0 -g", tls_define,
        out_path, tmppath,
        swc_dir, swc_dir,
        strip ? "-s" : "",
        extra_libs);

    if (nasts > 1)
        fprintf(stderr, "swc: compiling %d modules → %s\n", nasts, out_path);
    else
        fprintf(stderr, "swc: compiling %s → %s\n", input, out_path);
    int rc = system(cmd_buf);

    /* Cleanup temp file */
    if (!emit_c) swc_unlink(tmppath);

    if (rc != 0) {
        /* swc validates the program before emitting C, so C that fails to
         * compile is a compiler bug, not a user error — say so plainly
         * (the cc output above points at the .sw line via #line). */
        fprintf(stderr, "swc: internal compiler error: the generated C did not compile (cc returned %d).\n", rc);
        fprintf(stderr, "swc: this is a bug in swc, not in your program — please report it with the .sw file\n");
        fprintf(stderr, "swc: (rebuild with --emit-c to keep the generated C; command was: %s)\n", cmd_buf);
        return 1;
    }

    fprintf(stderr, "swc: built %s\n", out_path);
    return 0;
}
