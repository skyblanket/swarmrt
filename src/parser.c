/*
 * SwarmRT Parser - AI-friendly syntax
 * 
 * Syntax goals:
 * - Minimal punctuation
 * - Python-like indentation OR braces (AI prefers explicit)
 * - Swarm primitives as keywords
 * - No semicolons needed
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "swarmrt.h"

/* === Token Types === */
typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_ATOM,
    
    /* Keywords */
    TOK_SPAWN,
    TOK_RECEIVE,
    TOK_SEND,
    TOK_AFTER,
    TOK_SWARM,
    TOK_MAP,
    TOK_REDUCE,
    TOK_SUPERVISE,
    TOK_MODULE,
    TOK_EXPORT,
    TOK_FUN,
    TOK_WHEN,
    TOK_CASE,
    TOK_END,
    
    /* Operators */
    TOK_ARROW,      /* -> */
    TOK_FARROW,     /* => */
    TOK_PIPE,       /* |> */
    TOK_ASSIGN,     /* = */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_LT,         /* < */
    TOK_GT,         /* > */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    
    /* Delimiters */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,      /* , */
    TOK_DOT,        /* . */
    TOK_COLON,      /* : */
    TOK_SEMI,       /* ; */
    TOK_NEWLINE,
} token_type_t;

typedef struct {
    token_type_t type;
    char *text;
    int line;
    int col;
} token_t;

typedef struct {
    const char *source;
    int pos;
    int line;
    int col;
    token_t current;
} lexer_t;

/* === Keywords === */
static struct {
    const char *word;
    token_type_t type;
} keywords[] = {
    {"spawn", TOK_SPAWN},
    {"receive", TOK_RECEIVE},
    {"send", TOK_SEND},
    {"after", TOK_AFTER},
    {"swarm", TOK_SWARM},
    {"map", TOK_MAP},
    {"reduce", TOK_REDUCE},
    {"supervise", TOK_SUPERVISE},
    {"module", TOK_MODULE},
    {"export", TOK_EXPORT},
    {"fun", TOK_FUN},
    {"when", TOK_WHEN},
    {"case", TOK_CASE},
    {"end", TOK_END},
    {"true", TOK_ATOM},
    {"false", TOK_ATOM},
    {"nil", TOK_ATOM},
    {NULL, 0}
};

/* === Lexer === */
static void lexer_init(lexer_t *lex, const char *source) {
    lex->source = source;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
}

static char peek(lexer_t *lex) {
    return lex->source[lex->pos];
}

static char advance(lexer_t *lex) {
    char c = lex->source[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static void skip_whitespace(lexer_t *lex) {
    while (1) {
        char c = peek(lex);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lex);
        } else if (c == '#') {
            /* Comment to end of line */
            while (peek(lex) != '\n' && peek(lex) != '\0') {
                advance(lex);
            }
        } else {
            break;
        }
    }
}

static token_t make_token(lexer_t *lex, token_type_t type, const char *text) {
    token_t tok;
    tok.type = type;
    tok.text = strdup(text);
    tok.line = lex->line;
    tok.col = lex->col - strlen(text);
    return tok;
}

static token_t next_token(lexer_t *lex) {
    skip_whitespace(lex);
    
    char c = peek(lex);
    
    /* EOF */
    if (c == '\0') {
        return make_token(lex, TOK_EOF, "");
    }
    
    /* Newlines - skip them and get next token */
    if (c == '\n') {
        advance(lex);
        return next_token(lex);
    }
    
    /* Numbers */
    if (isdigit(c)) {
        char buf[64];
        int i = 0;
        while (isdigit(peek(lex)) || peek(lex) == '.') {
            buf[i++] = advance(lex);
        }
        buf[i] = '\0';
        return make_token(lex, TOK_NUMBER, buf);
    }
    
    /* Strings */
    if (c == '"') {
        advance(lex); /* " */
        char buf[1024];
        int i = 0;
        while (peek(lex) != '"' && peek(lex) != '\0') {
            buf[i++] = advance(lex);
        }
        buf[i] = '\0';
        advance(lex); /* " */
        return make_token(lex, TOK_STRING, buf);
    }
    
    /* Atoms (single quotes or lowercase identifier) */
    if (c == '\'') {
        advance(lex); /* ' */
        char buf[256];
        int i = 0;
        while (peek(lex) != '\'' && peek(lex) != '\0') {
            buf[i++] = advance(lex);
        }
        buf[i] = '\0';
        advance(lex); /* ' */
        return make_token(lex, TOK_ATOM, buf);
    }
    
    /* Identifiers and keywords */
    if (isalpha(c) || c == '_') {
        char buf[256];
        int i = 0;
        while (isalnum(peek(lex)) || peek(lex) == '_' || peek(lex) == '?') {
            buf[i++] = advance(lex);
        }
        buf[i] = '\0';
        
        /* Check keywords */
        for (int k = 0; keywords[k].word; k++) {
            if (strcmp(buf, keywords[k].word) == 0) {
                return make_token(lex, keywords[k].type, buf);
            }
        }
        
        return make_token(lex, TOK_IDENT, buf);
    }
    
    /* Two-char operators */
    if (c == '-' && lex->source[lex->pos + 1] == '>') {
        advance(lex); advance(lex);
        return make_token(lex, TOK_ARROW, "->");
    }
    if (c == '=' && lex->source[lex->pos + 1] == '>') {
        advance(lex); advance(lex);
        return make_token(lex, TOK_FARROW, "=>");
    }
    if (c == '=' && lex->source[lex->pos + 1] == '=') {
        advance(lex); advance(lex);
        return make_token(lex, TOK_EQ, "==");
    }
    if (c == '!' && lex->source[lex->pos + 1] == '=') {
        advance(lex); advance(lex);
        return make_token(lex, TOK_NEQ, "!=");
    }
    if (c == '|' && lex->source[lex->pos + 1] == '>') {
        advance(lex); advance(lex);
        return make_token(lex, TOK_PIPE, "|>");
    }
    
    /* Single-char operators */
    advance(lex);
    char buf[2] = {c, '\0'};
    
    switch (c) {
        case '=': return make_token(lex, TOK_ASSIGN, buf);
        case '<': return make_token(lex, TOK_LT, buf);
        case '>': return make_token(lex, TOK_GT, buf);
        case '+': return make_token(lex, TOK_PLUS, buf);
        case '-': return make_token(lex, TOK_MINUS, buf);
        case '*': return make_token(lex, TOK_STAR, buf);
        case '/': return make_token(lex, TOK_SLASH, buf);
        case '(': return make_token(lex, TOK_LPAREN, buf);
        case ')': return make_token(lex, TOK_RPAREN, buf);
        case '{': return make_token(lex, TOK_LBRACE, buf);
        case '}': return make_token(lex, TOK_RBRACE, buf);
        case '[': return make_token(lex, TOK_LBRACKET, buf);
        case ']': return make_token(lex, TOK_RBRACKET, buf);
        case ',': return make_token(lex, TOK_COMMA, buf);
        case '.': return make_token(lex, TOK_DOT, buf);
        case ':': return make_token(lex, TOK_COLON, buf);
        case ';': return make_token(lex, TOK_SEMI, buf);
        default:  return make_token(lex, TOK_EOF, buf);
    }
}

/* === AST Types === */
typedef enum {
    AST_MODULE,
    AST_FUNCTION,
    AST_SPAWN,
    AST_RECEIVE,
    AST_SEND,
    AST_CALL,
    AST_IDENT,
    AST_NUMBER,
    AST_STRING,
    AST_ATOM,
    AST_TUPLE,
    AST_LIST,
    AST_BINARY,
    AST_SWARM_MAP,
    AST_SWARM_REDUCE,
    AST_CASE,
    AST_CLAUSE,
    AST_BINARY_OP,
    AST_PIPE,
} ast_type_t;

typedef struct ast_node {
    ast_type_t type;
    int line;
    union {
        /* Module */
        struct {
            char *name;
            struct ast_node **exports;
            int num_exports;
            struct ast_node **functions;
            int num_functions;
        } module;
        
        /* Function */
        struct {
            char *name;
            char **params;
            int num_params;
            struct ast_node *body;
        } function;
        
        /* Spawn */
        struct {
            struct ast_node *func;
        } spawn;
        
        /* Receive */
        struct {
            struct ast_node **clauses;
            int num_clauses;
            int timeout;
            struct ast_node *timeout_body;
        } receive;
        
        /* Send */
        struct {
            struct ast_node *to;
            struct ast_node *msg;
        } send;
        
        /* Call */
        struct {
            struct ast_node *func;
            struct ast_node **args;
            int num_args;
        } call;
        
        /* Ident/Atom/String */
        char *name;
        
        /* Number */
        double number;
        
        /* Tuple/List */
        struct {
            struct ast_node **items;
            int count;
        } collection;
        
        /* Swarm map/reduce */
        struct {
            struct ast_node *func;
            struct ast_node *collection;
        } swarm_op;
        
        /* Case clause */
        struct {
            struct ast_node *pattern;
            struct ast_node *guard;
            struct ast_node *body;
        } clause;
        
        /* Binary op */
        struct {
            struct ast_node *left;
            struct ast_node *right;
            char op[4];
        } binary;
        
        /* Pipe */
        struct {
            struct ast_node *value;
            struct ast_node *func;
        } pipe;
    } val;
} ast_node_t;

/* === Parser === */
typedef struct {
    lexer_t *lex;
    token_t current;
    int had_error;
} parser_t;

static void parser_init(parser_t *parser, lexer_t *lex) {
    parser->lex = lex;
    parser->current = next_token(lex);
    parser->had_error = 0;
}

static token_t advance_parser(parser_t *parser) {
    token_t prev = parser->current;
    parser->current = next_token(parser->lex);
    return prev;
}

static int match(parser_t *parser, token_type_t type) {
    if (parser->current.type == type) {
        advance_parser(parser);
        return 1;
    }
    return 0;
}

static token_t consume(parser_t *parser, token_type_t type, const char *msg) {
    if (parser->current.type == type) {
        return advance_parser(parser);
    }
    
    fprintf(stderr, "Parse error at %d:%d: %s (got %s)\n",
            parser->current.line, parser->current.col,
            msg, parser->current.text);
    parser->had_error = 1;
    
    token_t dummy = {type, strdup(""), 0, 0};
    return dummy;
}

static ast_node_t *new_node(ast_type_t type) {
    ast_node_t *node = calloc(1, sizeof(ast_node_t));
    node->type = type;
    return node;
}

/* Forward declarations */
static ast_node_t *parse_expr(parser_t *parser);
static ast_node_t *parse_block(parser_t *parser);

static ast_node_t *parse_primary(parser_t *parser) {
    token_t tok = parser->current;
    
    switch (tok.type) {
        case TOK_NUMBER: {
            advance_parser(parser);
            ast_node_t *node = new_node(AST_NUMBER);
            node->val.number = atof(tok.text);
            free(tok.text);
            return node;
        }
        
        case TOK_STRING: {
            advance_parser(parser);
            ast_node_t *node = new_node(AST_STRING);
            node->val.name = tok.text;
            return node;
        }
        
        case TOK_ATOM: {
            advance_parser(parser);
            ast_node_t *node = new_node(AST_ATOM);
            node->val.name = tok.text;
            return node;
        }
        
        case TOK_IDENT: {
            advance_parser(parser);
            ast_node_t *node = new_node(AST_IDENT);
            node->val.name = tok.text;
            
            /* Function call? */
            if (match(parser, TOK_LPAREN)) {
                ast_node_t *call = new_node(AST_CALL);
                call->val.call.func = node;
                call->val.call.args = NULL;
                call->val.call.num_args = 0;
                
                /* Parse args */
                if (!match(parser, TOK_RPAREN)) {
                    do {
                        call->val.call.num_args++;
                        call->val.call.args = realloc(call->val.call.args,
                            sizeof(ast_node_t *) * call->val.call.num_args);
                        call->val.call.args[call->val.call.num_args - 1] = parse_expr(parser);
                    } while (match(parser, TOK_COMMA));
                    consume(parser, TOK_RPAREN, "Expected ')' after arguments");
                }
                return call;
            }
            
            return node;
        }
        
        case TOK_LBRACKET: {
            advance_parser(parser);
            ast_node_t *list = new_node(AST_LIST);
            list->val.collection.items = NULL;
            list->val.collection.count = 0;
            
            if (!match(parser, TOK_RBRACKET)) {
                do {
                    list->val.collection.count++;
                    list->val.collection.items = realloc(list->val.collection.items,
                        sizeof(ast_node_t *) * list->val.collection.count);
                    list->val.collection.items[list->val.collection.count - 1] = parse_expr(parser);
                } while (match(parser, TOK_COMMA));
                consume(parser, TOK_RBRACKET, "Expected ']' after list");
            }
            return list;
        }
        
        case TOK_LBRACE: {
            advance_parser(parser);
            ast_node_t *tuple = new_node(AST_TUPLE);
            tuple->val.collection.items = NULL;
            tuple->val.collection.count = 0;
            
            if (!match(parser, TOK_RBRACE)) {
                do {
                    tuple->val.collection.count++;
                    tuple->val.collection.items = realloc(tuple->val.collection.items,
                        sizeof(ast_node_t *) * tuple->val.collection.count);
                    tuple->val.collection.items[tuple->val.collection.count - 1] = parse_expr(parser);
                } while (match(parser, TOK_COMMA));
                consume(parser, TOK_RBRACE, "Expected '}' after tuple");
            }
            return tuple;
        }
        
        case TOK_LPAREN: {
            advance_parser(parser);
            ast_node_t *expr = parse_expr(parser);
            consume(parser, TOK_RPAREN, "Expected ')'");
            return expr;
        }
        
        default:
            fprintf(stderr, "Unexpected token: %s\n", tok.text);
            parser->had_error = 1;
            return new_node(AST_IDENT);
    }
}

static ast_node_t *parse_expr(parser_t *parser) {
    ast_node_t *left = parse_primary(parser);
    
    /* Pipe operator */
    while (match(parser, TOK_PIPE)) {
        ast_node_t *pipe = new_node(AST_PIPE);
        pipe->val.pipe.value = left;
        pipe->val.pipe.func = parse_primary(parser);
        left = pipe;
    }
    
    return left;
}

static ast_node_t *parse_function(parser_t *parser) {
    consume(parser, TOK_FUN, "Expected 'fun'");
    token_t name = consume(parser, TOK_IDENT, "Expected function name");
    
    consume(parser, TOK_LPAREN, "Expected '('");
    
    ast_node_t *func = new_node(AST_FUNCTION);
    func->val.function.name = name.text;
    func->val.function.params = NULL;
    func->val.function.num_params = 0;
    
    /* Parameters */
    if (!match(parser, TOK_RPAREN)) {
        do {
            token_t param = consume(parser, TOK_IDENT, "Expected parameter name");
            func->val.function.num_params++;
            func->val.function.params = realloc(func->val.function.params,
                sizeof(char *) * func->val.function.num_params);
            func->val.function.params[func->val.function.num_params - 1] = param.text;
        } while (match(parser, TOK_COMMA));
        consume(parser, TOK_RPAREN, "Expected ')'");
    }
    
    consume(parser, TOK_LBRACE, "Expected '{'");
    func->val.function.body = parse_expr(parser);
    consume(parser, TOK_RBRACE, "Expected '}'");
    
    return func;
}

static ast_node_t *parse_module(parser_t *parser) {
    consume(parser, TOK_MODULE, "Expected 'module'");
    token_t name = consume(parser, TOK_IDENT, "Expected module name");
    
    ast_node_t *mod = new_node(AST_MODULE);
    mod->val.module.name = name.text;
    mod->val.module.exports = NULL;
    mod->val.module.num_exports = 0;
    mod->val.module.functions = NULL;
    mod->val.module.num_functions = 0;
    
    /* Optional exports */
    if (match(parser, TOK_EXPORT)) {
        consume(parser, TOK_LBRACKET, "Expected '[' after export");
        if (!match(parser, TOK_RBRACKET)) {
            do {
                token_t exp = consume(parser, TOK_IDENT, "Expected export name");
                mod->val.module.num_exports++;
                mod->val.module.exports = realloc(mod->val.module.exports,
                    sizeof(ast_node_t *) * mod->val.module.num_exports);
                mod->val.module.exports[mod->val.module.num_exports - 1] = NULL;
                /* Store name somewhere */
            } while (match(parser, TOK_COMMA));
            consume(parser, TOK_RBRACKET, "Expected ']'");
        }
    }
    
    /* Functions */
    while (parser->current.type == TOK_FUN) {
        mod->val.module.num_functions++;
        mod->val.module.functions = realloc(mod->val.module.functions,
            sizeof(ast_node_t *) * mod->val.module.num_functions);
        mod->val.module.functions[mod->val.module.num_functions - 1] = parse_function(parser);
    }
    
    return mod;
}

ast_node_t *parse(const char *source) {
    lexer_t lex;
    lexer_init(&lex, source);
    
    parser_t parser;
    parser_init(&parser, &lex);
    
    ast_node_t *ast = parse_module(&parser);
    
    if (parser.had_error) {
        /* Cleanup */
        return NULL;
    }
    
    return ast;
}

/* === AST Printing (for debugging) === */
void print_ast(ast_node_t *node, int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
    
    if (!node) {
        printf("(null)\n");
        return;
    }
    
    switch (node->type) {
        case AST_MODULE:
            printf("module %s\n", node->val.module.name);
            for (int i = 0; i < node->val.module.num_functions; i++) {
                print_ast(node->val.module.functions[i], indent + 1);
            }
            break;
        
        case AST_FUNCTION:
            printf("fun %s(%d params)\n", node->val.function.name, node->val.function.num_params);
            print_ast(node->val.function.body, indent + 1);
            break;
        
        case AST_CALL:
            printf("call %s\n", node->val.call.func->val.name);
            for (int i = 0; i < node->val.call.num_args; i++) {
                print_ast(node->val.call.args[i], indent + 1);
            }
            break;
        
        case AST_IDENT:
            printf("ident: %s\n", node->val.name);
            break;
        
        case AST_NUMBER:
            printf("number: %f\n", node->val.number);
            break;
        
        case AST_STRING:
            printf("string: \"%s\"\n", node->val.name);
            break;
        
        case AST_ATOM:
            printf("atom: %s\n", node->val.name);
            break;
        
        case AST_LIST:
            printf("list[%d]\n", node->val.collection.count);
            for (int i = 0; i < node->val.collection.count; i++) {
                print_ast(node->val.collection.items[i], indent + 1);
            }
            break;
        
        default:
            printf("(unknown node type %d)\n", node->type);
            break;
    }
}

/* === Free AST === */
void free_ast(ast_node_t *node) {
    if (!node) return;
    
    /* TODO: recursively free everything */
    free(node);
}
