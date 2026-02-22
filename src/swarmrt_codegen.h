/*
 * SwarmRT Compiler: AST → C Code Generation
 *
 * Translates parsed .sw module ASTs into C source code that links
 * against the SwarmRT native runtime. Handles spawn trampolines,
 * receive/pattern-matching, tail call optimization, and pipe operators.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_CODEGEN_H
#define SWARMRT_CODEGEN_H

#include <stdio.h>

/*
 * Generate C code from a parsed module AST.
 *
 * ast:       result of sw_lang_parse() — a node_t* of type N_MODULE
 * out:       FILE* to write generated C code to
 * obfuscate: if non-zero, apply obfuscation passes after generation
 *
 * Returns 0 on success, -1 on error.
 */
int sw_codegen(void *ast, FILE *out, int obfuscate);

/*
 * Generate C code to a heap-allocated string.
 *
 * Returns malloc'd string with generated C code, or NULL on error.
 * Caller must free().
 */
char *sw_codegen_to_string(void *ast, int obfuscate);

/*
 * Apply obfuscation passes to generated C code.
 *
 * code:       the generated C source string
 * mod_name:   module name (e.g., "Counter")
 * func_names: array of function name strings
 * nfuncs:     number of functions
 *
 * Returns new malloc'd string with obfuscated code, or NULL on error.
 * Caller must free().
 */
char *sw_obfuscate(const char *code, const char *mod_name,
                   const char **func_names, int nfuncs);

#endif /* SWARMRT_CODEGEN_H */
