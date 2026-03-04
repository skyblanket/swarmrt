/*
 * SwarmRT REPL: Interactive expression evaluator
 *
 * Provides an interactive shell around sw_lang_eval() with persistent
 * interpreter state, multi-line input, and meta-commands.
 *
 * otonomy.ai
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swarmrt_repl.h"
#include "swarmrt_lang.h"

#define REPL_BUF_SIZE 8192
#define REPL_LINE_SIZE 1024

/* Count unmatched opening brackets/parens/braces */
static int bracket_depth(const char *s) {
    int depth = 0;
    int in_string = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (s[i] == '(' || s[i] == '[' || s[i] == '{') depth++;
        else if (s[i] == ')' || s[i] == ']' || s[i] == '}') depth--;
    }
    return depth;
}

static char *read_file(const char *path) {
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

static void print_help(void) {
    printf(
        "SwarmRT REPL — interactive expression evaluator\n\n"
        "Enter expressions to evaluate. Results are printed automatically.\n"
        "Multi-line input: unclosed brackets continue on the next line.\n\n"
        "Meta-commands:\n"
        "  :help          Show this help\n"
        "  :quit          Exit the REPL (also Ctrl-D)\n"
        "  :load <file>   Load and evaluate a .sw file\n"
        "  :env           Show current variable bindings\n"
        "  :reset         Reset interpreter state\n\n"
        "Examples:\n"
        "  sw> 2 + 2\n"
        "  4\n"
        "  sw> xs = [1, 2, 3]\n"
        "  [1, 2, 3]\n"
        "  sw> length(xs)\n"
        "  3\n");
}

static void print_env(sw_interp_t *interp) {
    sw_env_t *env = interp->global_env;
    if (!env) { printf("(empty)\n"); return; }
    int count = 0;
    for (int i = 0; i < SW_ENV_SLOTS; i++) {
        sw_env_entry_t *e = env->buckets[i];
        while (e) {
            printf("  %s = ", e->name);
            sw_val_print(e->val);
            printf("\n");
            count++;
            e = e->next;
        }
    }
    if (count == 0) printf("(empty)\n");
}

int sw_repl_start(void) {
    printf("SwarmRT REPL v0.1 — type :help for commands, :quit to exit\n\n");

    /* Create an empty module AST for the interpreter */
    void *ast = sw_lang_parse("module _REPL\n");
    if (!ast) {
        fprintf(stderr, "repl: failed to initialize\n");
        return 1;
    }
    sw_interp_t *interp = sw_lang_new(ast);

    char buf[REPL_BUF_SIZE];
    char line[REPL_LINE_SIZE];
    int continuing = 0;

    for (;;) {
        /* Prompt */
        if (continuing)
            printf("... ");
        else
            printf("sw> ");
        fflush(stdout);

        /* Read line */
        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF (Ctrl-D) */
            printf("\n");
            break;
        }

        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        /* If continuing, append to buffer */
        if (continuing) {
            size_t blen = strlen(buf);
            if (blen + len + 2 < REPL_BUF_SIZE) {
                buf[blen] = '\n';
                strcpy(buf + blen + 1, line);
            }
        } else {
            /* Skip empty lines */
            if (len == 0) continue;

            /* Meta-commands */
            if (line[0] == ':') {
                if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0)
                    break;
                if (strcmp(line, ":help") == 0 || strcmp(line, ":h") == 0) {
                    print_help();
                    continue;
                }
                if (strcmp(line, ":env") == 0) {
                    print_env(interp);
                    continue;
                }
                if (strcmp(line, ":reset") == 0) {
                    sw_lang_free(interp);
                    ast = sw_lang_parse("module _REPL\n");
                    interp = sw_lang_new(ast);
                    printf("state reset\n");
                    continue;
                }
                if (strncmp(line, ":load ", 6) == 0) {
                    const char *path = line + 6;
                    while (*path == ' ') path++;
                    char *source = read_file(path);
                    if (!source) {
                        printf("error: cannot open '%s'\n", path);
                        continue;
                    }
                    /* Parse and merge into interpreter */
                    void *mod = sw_lang_parse(source);
                    free(source);
                    if (!mod) {
                        printf("error: parse failed for '%s'\n", path);
                        continue;
                    }
                    /* Replace module AST so loaded functions are available */
                    void *old = interp->module_ast;
                    interp->module_ast = mod;
                    if (old) {
                        /* We don't free old — env may still reference AST nodes */
                    }
                    printf("loaded %s\n", path);
                    continue;
                }
                printf("unknown command: %s (try :help)\n", line);
                continue;
            }

            strncpy(buf, line, REPL_BUF_SIZE - 1);
            buf[REPL_BUF_SIZE - 1] = '\0';
        }

        /* Check for multi-line continuation */
        if (bracket_depth(buf) > 0) {
            continuing = 1;
            continue;
        }
        continuing = 0;

        /* Evaluate */
        interp->error = 0;
        interp->error_msg[0] = '\0';
        sw_val_t *result = sw_lang_eval_repl(interp, buf);

        if (interp->error) {
            printf("error: %s\n", interp->error_msg);
        } else if (result && result->type != SW_VAL_NIL) {
            sw_val_print(result);
            printf("\n");
        }
    }

    sw_lang_free(interp);
    return 0;
}
