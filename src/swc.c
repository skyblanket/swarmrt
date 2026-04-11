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

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
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
        "  emit     Output generated C to stdout\n"
        "  repl     Start interactive REPL\n"
        "  test     Run test_* functions in .sw files\n\n"
        "Options:\n"
        "  -o <name>     Output binary name (default: module name)\n"
        "  -O            Optimize (-O2)\n"
        "  --obfusc      Enable obfuscation (XOR strings + symbol mangle)\n"
        "  --strip       Strip symbols from binary\n"
        "  --emit-c      Save generated .c file (don't delete after compile)\n");
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

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* REPL — no input files needed */
    if (strcmp(cmd, "repl") == 0)
        return sw_repl_start();

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

    if (argc < 3) { usage(); return 1; }
    const char *inputs[64];
    int ninputs = 0;
    const char *output_name = NULL;
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

        /* Find which module has main() */
        const char *fnames[64];
        int nf = get_func_names(ast, fnames, 64);
        for (int j = 0; j < nf; j++)
            if (strcmp(fnames[j], "main") == 0) main_idx = nasts;
        nasts++;
    }

    /* Resolve imports: for each parsed AST, find import declarations and
     * auto-load the corresponding .sw files from the same directory */
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

                /* Try ModuleName.sw then modulename.sw */
                char imp_path[512];
                snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", input_dir, imp_name);
                char *imp_source = read_file(imp_path);
                if (!imp_source) {
                    /* Try lowercase */
                    char lower[128];
                    strncpy(lower, imp_name, sizeof(lower) - 1);
                    for (int c = 0; lower[c]; c++)
                        if (lower[c] >= 'A' && lower[c] <= 'Z') lower[c] += 32;
                    snprintf(imp_path, sizeof(imp_path), "%s/%s.sw", input_dir, lower);
                    imp_source = read_file(imp_path);
                }
                if (!imp_source) {
                    fprintf(stderr, "swc: cannot resolve import '%s'\n", imp_name);
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
    const char *extra_libs = "";
#else
    const char *extra_libs = "-lssl -lcrypto";
#endif
    snprintf(cmd_buf, sizeof(cmd_buf),
        "cc %s -fno-stack-protector -o %s %s -I%s/src -L%s/bin -lswarmrt -pthread %s %s",
        optimize ? "-O2" : "-O0 -g",
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
