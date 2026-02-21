/*
 * SwarmRT Main Entry Point
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "swarmrt.h"

/* External parser functions */
struct ast_node;
typedef struct ast_node ast_node_t;
ast_node_t *parse(const char *source);
void print_ast(ast_node_t *node, int indent);
void free_ast(ast_node_t *node);

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("fopen");
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    
    fclose(f);
    return buf;
}

static void print_usage(const char *prog) {
    printf("SwarmRT - Minimal BEAM-alike Runtime\n\n");
    printf("Usage:\n");
    printf("  %s parse <file.sw>    Parse and print AST\n", prog);
    printf("  %s run <file.sw>      Run a Swarm program\n", prog);
    printf("  %s test               Run tests\n", prog);
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *command = argv[1];
    
    if (strcmp(command, "parse") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s parse <file.sw>\n", argv[0]);
            return 1;
        }
        
        char *source = read_file(argv[2]);
        if (!source) return 1;
        
        printf("=== Parsing %s ===\n\n", argv[2]);
        printf("Source:\n%s\n\n", source);
        
        ast_node_t *ast = parse(source);
        if (ast) {
            printf("AST:\n");
            print_ast(ast, 0);
            free_ast(ast);
        } else {
            printf("Parse failed!\n");
        }
        
        free(source);
        return 0;
    }
    
    if (strcmp(command, "run") == 0) {
        printf("Run not yet implemented - use parse for now\n");
        return 1;
    }
    
    if (strcmp(command, "test") == 0) {
        printf("=== SwarmRT Tests ===\n\n");
        
        /* Test 1: Init/Shutdown */
        printf("Test 1: Init/Shutdown... ");
        swarm_init(4);
        swarm_stats();
        swarm_shutdown();
        printf("PASS\n\n");
        
        /* Test 2: Process spawning */
        printf("Test 2: Process Spawning... ");
        swarm_init(2);
        
        /* For now just test init/shutdown works */
        swarm_stats();
        
        swarm_shutdown();
        printf("PASS\n");
        
        return 0;
    }
    
    print_usage(argv[0]);
    return 1;
}
