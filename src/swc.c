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

static void usage(void) {
    fprintf(stderr,
        "Usage: swc <command> [options] <file.sw>\n\n"
        "Commands:\n"
        "  build    Compile .sw to native binary\n"
        "  run      Interpret a .sw file (calls main())\n"
        "  emit     Output generated C to stdout\n"
        "  repl     Start interactive REPL\n"
        "  test     Run test_* functions in .sw files\n\n"
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

/* `swc run <file.sw>` — interpret a .sw program: parse the file, resolve
 * its `import` declarations from the local dir and the bundled lib/, merge
 * all functions into one module, build an interpreter, and call main().
 * The documented interpreter run path (SW_LANGUAGE.md). Returns the
 * process exit code. */
static int run_file(const char *path, const char *argv0) {
    char *source = read_file(path);
    if (!source) return 1;
    void *main_ast = sw_lang_parse(source);
    free(source);
    if (!main_ast) { fprintf(stderr, "swc: parse failed for %s\n", path); return 1; }
    node_t *root = (node_t *)main_ast;

    /* swc-binary dir → <root>/lib fallback for stdlib imports. */
    char swc_dir[256] = ".";
    char swarmrt_lib[512] = "./lib";
    {
        char *sp = strdup(argv0);
        if (sp) {
            char *dir = dirname(sp);
            snprintf(swc_dir, sizeof(swc_dir), "%s", dir);
            free(sp);
        }
        snprintf(swarmrt_lib, sizeof(swarmrt_lib), "%s/../lib", swc_dir);
    }
    char input_dir[256] = ".";
    {
        char *tmp = strdup(path);
        if (tmp) { snprintf(input_dir, sizeof(input_dir), "%s", dirname(tmp)); free(tmp); }
    }

    /* Resolve + merge imports (same lookup order as the build path). */
    for (int im = 0; im < root->v.mod.nimports; im++) {
        const char *imp_name = root->v.mod.imports[im];
        char lower[128];
        snprintf(lower, sizeof(lower), "%s", imp_name);
        for (int c = 0; lower[c]; c++)
            if (lower[c] >= 'A' && lower[c] <= 'Z') lower[c] += 32;

        char imp_path[512];
        char *imp_source = NULL;
        const char *roots[2] = { input_dir, swarmrt_lib };
        for (int r = 0; r < 2 && !imp_source; r++) {
            snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", roots[r], imp_name);
            imp_source = read_file_quiet(imp_path);
            if (imp_source) break;
            snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", roots[r], lower);
            imp_source = read_file_quiet(imp_path);
        }
        if (!imp_source) {
            fprintf(stderr, "swc: cannot resolve import '%s' (looked in %s/ and %s/)\n",
                    imp_name, input_dir, swarmrt_lib);
            continue;
        }
        void *imp_ast = sw_lang_parse(imp_source);
        free(imp_source);
        if (!imp_ast) {
            fprintf(stderr, "swc: parse failed for import '%s'\n", imp_name);
            continue;
        }
        merge_module_funs(root, (node_t *)imp_ast, 1);
    }

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
    sw_lang_call(interp, "main", NULL, 0);
    int rc = interp->error ? 1 : 0;
    if (interp->error)
        fprintf(stderr, "swc: runtime error: %s\n", interp->error_msg);
    /* Don't node_free the merged module: its funs[] mixes shared imported
     * nodes with shallow qualified clones (sharing bodies), so a recursive
     * free would double-free. Same trade-off the test runner makes — let
     * process exit reclaim the AST. */
    interp->module_ast = NULL;
    sw_lang_free(interp);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* REPL — no input files needed */
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
        return run_file(argv[2], argv[0]);
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
    int nasts = 0;
    int main_idx = 0;

    for (int i = 0; i < ninputs; i++) {
        char *source = read_file(inputs[i]);
        if (!source) return 1;
        void *ast = sw_lang_parse(source);
        free(source);
        if (!ast) { fprintf(stderr, "swc: parse failed for %s\n", inputs[i]); return 1; }
        asts[nasts] = ast;
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

    /* Resolve imports: for each parsed AST, find import declarations and
     * auto-load the corresponding .sw files. Lookup order:
     *   1. <input_dir>/<Name>.sw  (capitalised — matches `import Foo`)
     *   2. <input_dir>/<name>.sw  (lowercase — legacy `main.sw` style)
     *   3. <swarmrt_root>/lib/<Name>.sw   (stdlib path)
     *   4. <swarmrt_root>/lib/<name>.sw   (stdlib path, lowercase)
     */
    {
        char input_dir[256];
        char *tmp = strdup(inputs[0]);
#ifdef _WIN32
        /* Manual dirname: find last separator */
        char *sep = strrchr(tmp, '/');
        char *bsep = strrchr(tmp, '\\');
        if (bsep > sep) sep = bsep;
        if (sep) *sep = '\0'; else tmp[0] = '.', tmp[1] = '\0';
        strncpy(input_dir, tmp, sizeof(input_dir) - 1);
#else
        char *dir = dirname(tmp);
        strncpy(input_dir, dir, sizeof(input_dir) - 1);
#endif
        free(tmp);

        for (int a = 0; a < nasts; a++) {
            node_t *mod = (node_t *)asts[a];
            for (int im = 0; im < mod->v.mod.nimports; im++) {
                const char *imp_name = mod->v.mod.imports[im];
                /* Check if already loaded */
                int found = 0;
                for (int e = 0; e < nasts; e++) {
                    if (strcmp(get_mod_name(asts[e]), imp_name) == 0) { found = 1; break; }
                }
                if (found) continue;

                /* Lowercase variant computed once. */
                char lower[128];
                strncpy(lower, imp_name, sizeof(lower) - 1);
                for (int c = 0; lower[c]; c++)
                    if (lower[c] >= 'A' && lower[c] <= 'Z') lower[c] += 32;

                /* Search in order: input_dir CamelCase → input_dir lower
                 *   → stdlib CamelCase → stdlib lower */
                char imp_path[512];
                char *imp_source = NULL;
                const char *roots[2] = { input_dir, swarmrt_lib };
                /* Probe candidate paths SILENTLY — a miss on any individual
                 * candidate is expected (we try CamelCase + lowercase across
                 * input_dir + stdlib). Only a *total* failure (all four
                 * candidates missed) is a genuine error, reported below.
                 * read_file (noisy) would otherwise spam "cannot open
                 * './Std.sw'" on every SUCCESSFUL stdlib import. */
                for (int r = 0; r < 2 && !imp_source; r++) {
                    snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", roots[r], imp_name);
                    imp_source = read_file_quiet(imp_path);
                    if (imp_source) break;
                    snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", roots[r], lower);
                    imp_source = read_file_quiet(imp_path);
                }
                if (!imp_source) {
                    fprintf(stderr, "swc: cannot resolve import '%s' (looked in %s/ and %s/)\n",
                            imp_name, input_dir, swarmrt_lib);
                    continue;
                }
                void *imp_ast = sw_lang_parse(imp_source);
                free(imp_source);
                if (!imp_ast) {
                    fprintf(stderr, "swc: parse failed for import '%s'\n", imp_name);
                    continue;
                }
                if (nasts < 64) {
                    asts[nasts++] = imp_ast;
                    sw_codegen_register_source(get_mod_name(imp_ast), imp_path);
                    fprintf(stderr, "swc: auto-imported %s from %s\n", imp_name, imp_path);
                }
            }
        }
    }

    const char *mod_name = get_mod_name(asts[main_idx]);

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
     * When the host was built with -DSWARMRT_TLS, generated binaries that
     * call wsc_connect_tls(wss://) also need OpenSSL linked. We pass the
     * Homebrew openssl@3 prefix that the Makefile used. */
  #ifdef SWARMRT_TLS
    const char *extra_libs =
        "-lsqlite3 -lm "
        "-L" SWARMRT_OPENSSL_PREFIX "/lib -lssl -lcrypto "
        "-I" SWARMRT_OPENSSL_PREFIX "/include";
  #else
    const char *extra_libs = "-lsqlite3 -lm";
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
        fprintf(stderr, "swc: compilation failed (cc returned %d)\n", rc);
        fprintf(stderr, "swc: command was: %s\n", cmd_buf);
        return 1;
    }

    fprintf(stderr, "swc: built %s\n", out_path);
    return 0;
}
