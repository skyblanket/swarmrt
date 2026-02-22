/*
 * SwarmRT Phase 10: Language Frontend Tests
 *
 * Tests the .sw language parser, evaluator, and runtime integration.
 *
 * 8 tests: parse module, arithmetic, strings, functions, tuples/lists,
 *          pattern match, pipe operator, if/else.
 */

#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "swarmrt_lang.h"
#include "swarmrt_otp.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_PASS(name) do { tests_passed++; printf("  [PASS] %s\n", name); } while(0)
#define TEST_FAIL(name) do { tests_failed++; printf("  [FAIL] %s\n", name); } while(0)
#define TEST_CHECK(name, cond) do { if (cond) TEST_PASS(name); else TEST_FAIL(name); } while(0)

/* =========================================================================
 * L1: Parse a .sw module
 * ========================================================================= */

static void test_parse_module(void) {
    printf("\n=== L1: Parse module ===\n");

    const char *src =
        "module Hello\n"
        "export [main]\n"
        "fun main() {\n"
        "    print(\"Hello, Swarm!\")\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    int parsed = (ast != NULL);
    printf("  Parsed: %s\n", parsed ? "YES" : "NO");

    if (parsed) {
        sw_interp_t *interp = sw_lang_new(ast);
        sw_val_t *result = sw_lang_call(interp, "main", NULL, 0);
        int ok = (result != NULL && result->type == SW_VAL_ATOM &&
                  strcmp(result->v.str, "ok") == 0);
        printf("  main() returned: ");
        sw_val_print(result);
        printf("\n");
        TEST_CHECK("parse_module", ok);
        /* interp owns ast, so don't double-free */
        sw_lang_free(interp);
    } else {
        TEST_FAIL("parse_module");
    }
}

/* =========================================================================
 * L2: Integer arithmetic
 * ========================================================================= */

static void test_arithmetic(void) {
    printf("\n=== L2: Arithmetic ===\n");

    const char *src =
        "module Math\n"
        "fun add(a, b) {\n"
        "    a + b\n"
        "}\n"
        "fun complex(x) {\n"
        "    (x * 2) + 3\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    /* Test add(10, 32) = 42 */
    sw_val_t *args1[2] = { sw_val_int(10), sw_val_int(32) };
    sw_val_t *r1 = sw_lang_call(interp, "add", args1, 2);

    /* Test complex(5) = 13 */
    sw_val_t *args2[1] = { sw_val_int(5) };
    sw_val_t *r2 = sw_lang_call(interp, "complex", args2, 1);

    printf("  add(10, 32) = "); sw_val_print(r1); printf("\n");
    printf("  complex(5) = "); sw_val_print(r2); printf("\n");

    int passed = (r1 && r1->type == SW_VAL_INT && r1->v.i == 42 &&
                  r2 && r2->type == SW_VAL_INT && r2->v.i == 13);
    TEST_CHECK("arithmetic", passed);

    sw_val_free(args1[0]); sw_val_free(args1[1]);
    sw_val_free(args2[0]);
    sw_lang_free(interp);
}

/* =========================================================================
 * L3: String operations
 * ========================================================================= */

static void test_strings(void) {
    printf("\n=== L3: Strings ===\n");

    const char *src =
        "module Strings\n"
        "fun greet(name) {\n"
        "    \"Hello, \" ++ name ++ \"!\"\n"
        "}\n"
        "fun len(s) {\n"
        "    length(s)\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *args1[1] = { sw_val_string("World") };
    sw_val_t *r1 = sw_lang_call(interp, "greet", args1, 1);

    sw_val_t *args2[1] = { sw_val_string("hello") };
    sw_val_t *r2 = sw_lang_call(interp, "len", args2, 1);

    printf("  greet(\"World\") = "); sw_val_print(r1); printf("\n");
    printf("  len(\"hello\") = "); sw_val_print(r2); printf("\n");

    int passed = (r1 && r1->type == SW_VAL_STRING &&
                  strcmp(r1->v.str, "Hello, World!") == 0 &&
                  r2 && r2->type == SW_VAL_INT && r2->v.i == 5);
    TEST_CHECK("strings", passed);

    sw_val_free(args1[0]);
    sw_val_free(args2[0]);
    sw_lang_free(interp);
}

/* =========================================================================
 * L4: Function calls (user-defined + recursive)
 * ========================================================================= */

static void test_functions(void) {
    printf("\n=== L4: Functions ===\n");

    const char *src =
        "module Funcs\n"
        "fun double(x) {\n"
        "    x * 2\n"
        "}\n"
        "fun quad(x) {\n"
        "    double(double(x))\n"
        "}\n"
        "fun factorial(n) {\n"
        "    if (n <= 1) {\n"
        "        1\n"
        "    } else {\n"
        "        n * factorial(n - 1)\n"
        "    }\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *args1[1] = { sw_val_int(5) };
    sw_val_t *r1 = sw_lang_call(interp, "quad", args1, 1);

    sw_val_t *args2[1] = { sw_val_int(6) };
    sw_val_t *r2 = sw_lang_call(interp, "factorial", args2, 1);

    printf("  quad(5) = "); sw_val_print(r1); printf("\n");
    printf("  factorial(6) = "); sw_val_print(r2); printf("\n");

    int passed = (r1 && r1->type == SW_VAL_INT && r1->v.i == 20 &&
                  r2 && r2->type == SW_VAL_INT && r2->v.i == 720);
    TEST_CHECK("functions", passed);

    sw_val_free(args1[0]);
    sw_val_free(args2[0]);
    sw_lang_free(interp);
}

/* =========================================================================
 * L5: Tuples and lists
 * ========================================================================= */

static void test_collections(void) {
    printf("\n=== L5: Tuples & Lists ===\n");

    const char *src =
        "module Collections\n"
        "fun make_tuple() {\n"
        "    {1, 2, 3}\n"
        "}\n"
        "fun make_list() {\n"
        "    [10, 20, 30]\n"
        "}\n"
        "fun first(lst) {\n"
        "    hd(lst)\n"
        "}\n"
        "fun count(lst) {\n"
        "    length(lst)\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *r1 = sw_lang_call(interp, "make_tuple", NULL, 0);
    sw_val_t *r2 = sw_lang_call(interp, "make_list", NULL, 0);

    sw_val_t *args3[1] = { r2 };
    sw_val_t *r3 = sw_lang_call(interp, "first", args3, 1);

    sw_val_t *args4[1] = { r2 };
    sw_val_t *r4 = sw_lang_call(interp, "count", args4, 1);

    printf("  make_tuple() = "); sw_val_print(r1); printf("\n");
    printf("  make_list() = "); sw_val_print(r2); printf("\n");
    printf("  first(list) = "); sw_val_print(r3); printf("\n");
    printf("  count(list) = "); sw_val_print(r4); printf("\n");

    int passed = (r1 && r1->type == SW_VAL_TUPLE && r1->v.tuple.count == 3 &&
                  r2 && r2->type == SW_VAL_LIST && r2->v.tuple.count == 3 &&
                  r3 && r3->type == SW_VAL_INT && r3->v.i == 10 &&
                  r4 && r4->type == SW_VAL_INT && r4->v.i == 3);
    TEST_CHECK("collections", passed);

    sw_lang_free(interp);
}

/* =========================================================================
 * L6: Eval expression strings
 * ========================================================================= */

static void test_eval(void) {
    printf("\n=== L6: Eval expressions ===\n");

    const char *dummy_src = "module Dummy\nfun noop() { nil }\n";
    void *ast = sw_lang_parse(dummy_src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *r1 = sw_lang_eval(interp, "2 + 3 * 4");
    sw_val_t *r2 = sw_lang_eval(interp, "\"hello\" ++ \" \" ++ \"world\"");
    sw_val_t *r3 = sw_lang_eval(interp, "10 > 5");

    printf("  2 + 3 * 4 = "); sw_val_print(r1); printf("\n");
    printf("  string concat = "); sw_val_print(r2); printf("\n");
    printf("  10 > 5 = "); sw_val_print(r3); printf("\n");

    /* 2 + 3 * 4 = 14 (with proper precedence: 3*4=12, 2+12=14) */
    int passed = (r1 && r1->type == SW_VAL_INT && r1->v.i == 14 &&
                  r2 && r2->type == SW_VAL_STRING && strcmp(r2->v.str, "hello world") == 0 &&
                  r3 && r3->type == SW_VAL_ATOM && strcmp(r3->v.str, "true") == 0);
    TEST_CHECK("eval", passed);

    sw_lang_free(interp);
}

/* =========================================================================
 * L7: Pipe operator
 * ========================================================================= */

static void test_pipe(void) {
    printf("\n=== L7: Pipe operator ===\n");

    const char *src =
        "module Pipes\n"
        "fun double(x) {\n"
        "    x * 2\n"
        "}\n"
        "fun add_one(x) {\n"
        "    x + 1\n"
        "}\n"
        "fun pipeline(x) {\n"
        "    x |> double() |> add_one()\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *args[1] = { sw_val_int(5) };
    sw_val_t *r = sw_lang_call(interp, "pipeline", args, 1);

    printf("  5 |> double() |> add_one() = "); sw_val_print(r); printf("\n");

    /* 5 * 2 = 10, 10 + 1 = 11 */
    int passed = (r && r->type == SW_VAL_INT && r->v.i == 11);
    TEST_CHECK("pipe", passed);

    sw_val_free(args[0]);
    sw_lang_free(interp);
}

/* =========================================================================
 * L8: If/else
 * ========================================================================= */

static void test_if_else(void) {
    printf("\n=== L8: If/else ===\n");

    const char *src =
        "module Logic\n"
        "fun max(a, b) {\n"
        "    if (a > b) {\n"
        "        a\n"
        "    } else {\n"
        "        b\n"
        "    }\n"
        "}\n"
        "fun sign(x) {\n"
        "    if (x > 0) {\n"
        "        \"positive\"\n"
        "    } else {\n"
        "        if (x < 0) {\n"
        "            \"negative\"\n"
        "        } else {\n"
        "            \"zero\"\n"
        "        }\n"
        "    }\n"
        "}\n";

    void *ast = sw_lang_parse(src);
    sw_interp_t *interp = sw_lang_new(ast);

    sw_val_t *args1[2] = { sw_val_int(10), sw_val_int(20) };
    sw_val_t *r1 = sw_lang_call(interp, "max", args1, 2);

    sw_val_t *args2[1] = { sw_val_int(-5) };
    sw_val_t *r2 = sw_lang_call(interp, "sign", args2, 1);

    sw_val_t *args3[1] = { sw_val_int(0) };
    sw_val_t *r3 = sw_lang_call(interp, "sign", args3, 1);

    printf("  max(10, 20) = "); sw_val_print(r1); printf("\n");
    printf("  sign(-5) = "); sw_val_print(r2); printf("\n");
    printf("  sign(0) = "); sw_val_print(r3); printf("\n");

    int passed = (r1 && r1->type == SW_VAL_INT && r1->v.i == 20 &&
                  r2 && r2->type == SW_VAL_STRING && strcmp(r2->v.str, "negative") == 0 &&
                  r3 && r3->type == SW_VAL_STRING && strcmp(r3->v.str, "zero") == 0);
    TEST_CHECK("if_else", passed);

    sw_val_free(args1[0]); sw_val_free(args1[1]);
    sw_val_free(args2[0]);
    sw_val_free(args3[0]);
    sw_lang_free(interp);
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("============================================\n");
    printf("  SwarmRT Phase 10: Language Frontend\n");
    printf("============================================\n");

    if (sw_init("phase10-test", 4) != 0) {
        fprintf(stderr, "Failed to initialize SwarmRT\n");
        return 1;
    }

    test_parse_module();
    test_arithmetic();
    test_strings();
    test_functions();
    test_collections();
    test_eval();
    test_pipe();
    test_if_else();

    sw_stats(0);
    sw_shutdown(0);

    printf("\n============================================\n");
    printf("  Results: %d passed, %d failed (of %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
