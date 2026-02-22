/*
 * SwarmRT Compiler CLI: swc
 *
 * Usage:
 *   swc build <file.sw> [-o name] [-O] [--obfusc] [--strip] [--emit-c]
 *   swc emit  <file.sw>
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
#include <unistd.h>
#include <libgen.h>
#include "swarmrt_lang.h"
#include "swarmrt_codegen.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: swc <command> [options] <file.sw>\n\n"
        "Commands:\n"
        "  build    Compile .sw to native binary\n"
        "  emit     Output generated C to stdout\n\n"
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
    if (argc < 3) { usage(); return 1; }

    const char *cmd = argv[1];
    const char *input = NULL;
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
        else if (argv[i][0] != '-')
            input = argv[i];
        else {
            fprintf(stderr, "swc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input) { fprintf(stderr, "swc: no input file\n"); return 1; }

    /* Read source */
    char *source = read_file(input);
    if (!source) return 1;

    /* Parse */
    void *ast = sw_lang_parse(source);
    free(source);
    if (!ast) { fprintf(stderr, "swc: parse failed\n"); return 1; }

    const char *mod_name = get_mod_name(ast);

    /* ---- emit command ---- */
    if (strcmp(cmd, "emit") == 0) {
        char *code = sw_codegen_to_string(ast, 0);
        if (!code) { fprintf(stderr, "swc: codegen failed\n"); return 1; }
        if (obfusc) {
            const char *fnames[64];
            int nfuncs = get_func_names(ast, fnames, 64);
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
    char *code = sw_codegen_to_string(ast, 0);
    if (!code) { fprintf(stderr, "swc: codegen failed\n"); return 1; }

    /* Apply obfuscation if requested */
    if (obfusc) {
        const char *fnames[64];
        int nfuncs = get_func_names(ast, fnames, 64);
        char *obf = sw_obfuscate(code, mod_name, fnames, nfuncs);
        free(code);
        code = obf;
    }

    /* Write to temp file */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/swc_%s_XXXXXX.c", mod_name);
    /* mkstemps needs suffix length */
    int fd = mkstemps(tmppath, 2);
    if (fd < 0) {
        /* Fallback: use fixed name */
        snprintf(tmppath, sizeof(tmppath), "/tmp/swc_%s.c", mod_name);
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
        char *dir = dirname(swc_path);
        /* Go up from bin/ to project root */
        snprintf(swc_dir, sizeof(swc_dir), "%s/..", dir);
        free(swc_path);
    }

    /* Compile with cc */
    char cmd_buf[2048];
    snprintf(cmd_buf, sizeof(cmd_buf),
        "cc %s -o %s %s -I%s/src -L%s/bin -lswarmrt -pthread %s",
        optimize ? "-O2" : "-O0 -g",
        out_path, tmppath,
        swc_dir, swc_dir,
        strip ? "-s" : "");

    fprintf(stderr, "swc: compiling %s → %s\n", input, out_path);
    int rc = system(cmd_buf);

    /* Cleanup temp file */
    if (!emit_c) unlink(tmppath);

    if (rc != 0) {
        fprintf(stderr, "swc: compilation failed (cc returned %d)\n", rc);
        fprintf(stderr, "swc: command was: %s\n", cmd_buf);
        return 1;
    }

    fprintf(stderr, "swc: built %s\n", out_path);
    return 0;
}
