/*
 * coder.c — "mally-like" coding agent CLI, powered by Gemma 4 (or any
 * OpenAI-compatible chat completions endpoint, e.g. vLLM).
 *
 * Tools: bash, read, write, edit. That's it. A minimal but fully functional
 * coding harness you can use on a real codebase.
 *
 * Usage:
 *   coder                                    (defaults: http://sushi:8000, gemma4)
 *   coder --endpoint http://sushi:8000 --model google/gemma-4-31B-it
 *   coder --system "You are a terse assistant..."
 *   coder --verbose
 *
 * At the prompt, just type what you want. Ctrl-D or `/quit` to exit.
 * `/reset` clears conversation history (keeping the system prompt).
 */
#include "llm.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

/* ANSI colours (disabled if stdout isn't a tty). */
static int use_colour = 0;
#define C_RESET (use_colour ? "\033[0m"  : "")
#define C_DIM   (use_colour ? "\033[2m"  : "")
#define C_BOLD  (use_colour ? "\033[1m"  : "")
#define C_BLUE  (use_colour ? "\033[34m" : "")
#define C_GREEN (use_colour ? "\033[32m" : "")
#define C_YEL   (use_colour ? "\033[33m" : "")
#define C_CYAN  (use_colour ? "\033[36m" : "")
#define C_MAG   (use_colour ? "\033[35m" : "")

/* ------------------------------------------------------------
 * Conversation history (dynamic array of llm_message_t)
 * ------------------------------------------------------------ */
typedef struct {
    llm_message_t *msgs;
    int count;
    int cap;
    int system_count; /* first N messages are the system prompt; /reset keeps them */
} history_t;

static void hist_init(history_t *h) {
    h->msgs = NULL; h->count = 0; h->cap = 0; h->system_count = 0;
}
static void hist_grow(history_t *h) {
    if (h->count + 1 <= h->cap) return;
    h->cap = h->cap ? h->cap * 2 : 16;
    h->msgs = realloc(h->msgs, sizeof(llm_message_t) * h->cap);
}
static llm_message_t *hist_push_empty(history_t *h) {
    hist_grow(h);
    llm_message_t *m = &h->msgs[h->count++];
    memset(m, 0, sizeof *m);
    return m;
}
static void hist_push_user(history_t *h, const char *text) {
    llm_message_t *m = hist_push_empty(h);
    strcpy(m->role, "user");
    m->content = strdup(text);
}
static void hist_push_system(history_t *h, const char *text) {
    llm_message_t *m = hist_push_empty(h);
    strcpy(m->role, "system");
    m->content = strdup(text);
    h->system_count = h->count;
}
static void hist_push_tool_result(history_t *h, const char *call_id, const char *result) {
    llm_message_t *m = hist_push_empty(h);
    strcpy(m->role, "tool");
    m->content = strdup(result);
    m->tool_call_id = strdup(call_id);
}
/* Move an assistant message (produced by llm_chat) into the history, transferring
 * ownership of its heap fields. */
static void hist_push_assistant(history_t *h, llm_message_t *src) {
    llm_message_t *m = hist_push_empty(h);
    *m = *src;
    strcpy(m->role, "assistant");
    memset(src, 0, sizeof *src); /* ownership transferred */
}
static void hist_reset(history_t *h) {
    for (int i = h->system_count; i < h->count; i++) llm_message_free(&h->msgs[i]);
    h->count = h->system_count;
}
static void hist_free(history_t *h) {
    for (int i = 0; i < h->count; i++) llm_message_free(&h->msgs[i]);
    free(h->msgs);
    h->msgs = NULL; h->count = h->cap = 0;
}

/* ------------------------------------------------------------
 * Default system prompt
 *
 * We teach the model a clean prompt-based tool-calling protocol:
 *    <tool_call>{"name":"bash","arguments":{"command":"ls"}}</tool_call>
 * This works regardless of whether the server supports OpenAI-style `tools`
 * (transformers serve, for instance, does not yet parse Gemma 4's native
 * tool format into OpenAI `tool_calls`). The client scans the assistant
 * content for these blocks after each turn.
 * ------------------------------------------------------------ */
static const char *SYSTEM_PREAMBLE =
    "You are coder, a concise, direct coding assistant running in a terminal. "
    "You have real tools that execute on the user's machine — use them to do "
    "work instead of just describing what you would do. Be terse. Prefer small, "
    "reversible actions. Never run destructive commands (rm -rf, force pushes, "
    "dropping tables) without explicit user confirmation.\n"
    "\n"
    "=== TOOL-CALLING PROTOCOL ===\n"
    "When you want to call a tool, emit EXACTLY ONE line formatted as:\n"
    "<tool_call>{\"name\":\"TOOL_NAME\",\"arguments\":{...}}</tool_call>\n"
    "Then STOP. Do not write anything after the closing </tool_call> tag — the "
    "system will execute the tool and send you the result in the next turn.\n"
    "You may write a brief one-line rationale BEFORE the tool_call if helpful.\n"
    "Arguments must be valid JSON. Use only the tools listed below.\n"
    "When the task is complete and you have no more tool calls, reply in plain "
    "prose with a short summary (no tool_call block).\n"
    "\n"
    "=== AVAILABLE TOOLS ===\n";

/* Build a system prompt that combines the preamble with tool schemas. */
static char *build_system_prompt(const char *user_override) {
    size_t cap = 8192;
    char *out = malloc(cap);
    size_t len = 0;
    #define APPEND(txt) do { \
        size_t n = strlen(txt); \
        while (len + n + 1 > cap) { cap *= 2; out = realloc(out, cap); } \
        memcpy(out + len, txt, n); len += n; out[len] = '\0'; \
    } while (0)

    APPEND(SYSTEM_PREAMBLE);
    for (int i = 0; i < CODER_TOOLS_COUNT; i++) {
        APPEND("- ");
        APPEND(CODER_TOOLS[i].name);
        APPEND(": ");
        APPEND(CODER_TOOLS[i].description);
        APPEND("\n  schema: ");
        APPEND(CODER_TOOLS[i].params_schema_json);
        APPEND("\n");
    }
    if (user_override && *user_override) {
        APPEND("\n=== USER EXTRA INSTRUCTIONS ===\n");
        APPEND(user_override);
    }
    return out;
    #undef APPEND
}

/* ------------------------------------------------------------
 * Extract <tool_call>{...}</tool_call> blocks from assistant
 * content and populate msg->tool_calls. Trims the blocks from
 * content so only the rationale (if any) is shown to the user.
 * Returns number of tool calls extracted.
 * ------------------------------------------------------------ */
static int extract_tool_calls(llm_message_t *m) {
    if (!m->content) return 0;
    const char *OPEN = "<tool_call>";
    const char *CLOSE = "</tool_call>";
    const size_t OLEN = 11; /* strlen("<tool_call>") */
    const size_t CLEN = 12; /* strlen("</tool_call>") */

    char *cur = m->content;
    int found = 0;
    while (found < LLM_MAX_TOOL_CALLS) {
        char *start = strstr(cur, OPEN);
        if (!start) break;
        char *json_start = start + OLEN;
        char *end = strstr(json_start, CLOSE);
        if (!end) break;

        /* Skip leading whitespace inside the block. */
        char *js = json_start;
        while (js < end && (*js == ' ' || *js == '\n' || *js == '\r' || *js == '\t')) js++;

        /* Parse the "name" field. */
        const char *nkey = strstr(js, "\"name\"");
        if (nkey && nkey < end) {
            const char *q1 = strchr(nkey + 6, '"');
            if (q1 && q1 < end) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && q2 < end) {
                    size_t nlen = (size_t)(q2 - q1 - 1);
                    if (nlen >= sizeof m->tool_calls[found].name)
                        nlen = sizeof m->tool_calls[found].name - 1;
                    memcpy(m->tool_calls[found].name, q1 + 1, nlen);
                    m->tool_calls[found].name[nlen] = '\0';
                }
            }
        }

        /* Parse the "arguments" object: find the key, skip to '{', match braces. */
        const char *akey = strstr(js, "\"arguments\"");
        if (akey && akey < end) {
            const char *colon = strchr(akey, ':');
            if (colon && colon < end) {
                const char *p = colon + 1;
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
                if (p < end && *p == '{') {
                    int depth = 0;
                    const char *q = p;
                    int in_str = 0;
                    while (q < end) {
                        if (in_str) {
                            if (*q == '\\' && q + 1 < end) q++;
                            else if (*q == '"') in_str = 0;
                        } else {
                            if (*q == '"') in_str = 1;
                            else if (*q == '{') depth++;
                            else if (*q == '}') { depth--; if (depth == 0) break; }
                        }
                        q++;
                    }
                    if (depth == 0 && q < end) {
                        size_t alen = (size_t)(q - p + 1);
                        m->tool_calls[found].arguments_json = malloc(alen + 1);
                        memcpy(m->tool_calls[found].arguments_json, p, alen);
                        m->tool_calls[found].arguments_json[alen] = '\0';
                    }
                }
            }
        }

        snprintf(m->tool_calls[found].id, sizeof m->tool_calls[found].id,
                 "call_%d", found);
        if (!m->tool_calls[found].arguments_json)
            m->tool_calls[found].arguments_json = strdup("{}");

        found++;
        cur = end + CLEN;
    }

    if (found > 0) {
        /* Strip everything from the first <tool_call> onward from content. */
        char *first = strstr(m->content, OPEN);
        if (first) {
            *first = '\0';
            /* Trim trailing whitespace. */
            char *e = first - 1;
            while (e >= m->content &&
                   (*e == ' ' || *e == '\n' || *e == '\r' || *e == '\t')) {
                *e = '\0'; e--;
            }
            if (m->content[0] == '\0') { free(m->content); m->content = NULL; }
        }
    }

    m->num_tool_calls = found;
    return found;
}

/* ------------------------------------------------------------
 * One conversational turn: run the agent loop until assistant
 * produces a final text response (no more tool calls).
 * ------------------------------------------------------------ */
static int run_turn(const llm_client_t *c, history_t *h) {
    /* Cap iterations to prevent runaway tool loops. */
    const int MAX_STEPS = 20;
    for (int step = 0; step < MAX_STEPS; step++) {
        llm_message_t assistant = {0};
        /* We pass 0 tools here — tool schemas are injected into the system
         * prompt and tool calls are parsed from content (see extract_tool_calls). */
        int rc = llm_chat(c, h->msgs, h->count, NULL, 0, &assistant);
        if (rc != 0) {
            fprintf(stderr, "%s[error] llm_chat failed%s\n", C_YEL, C_RESET);
            llm_message_free(&assistant);
            return -1;
        }

        /* Extract any <tool_call>...</tool_call> blocks from content. */
        extract_tool_calls(&assistant);

        /* Show any prose content from the model. */
        if (assistant.content && assistant.content[0]) {
            printf("%s%s%s\n", C_BOLD, assistant.content, C_RESET);
            fflush(stdout);
        }

        int ntc = assistant.num_tool_calls;
        /* Commit assistant message to history (transfers ownership). */
        hist_push_assistant(h, &assistant);

        if (ntc == 0) return 0; /* done: final answer produced */

        /* Execute each tool call and append its result. */
        for (int i = 0; i < ntc; i++) {
            llm_tool_call_t *tc = &h->msgs[h->count - 1].tool_calls[i];
            printf("%s· %s(%s)%s\n",
                   C_CYAN, tc->name, tc->arguments_json ? tc->arguments_json : "",
                   C_RESET);
            fflush(stdout);

            char *result = coder_tool_exec(tc->name, tc->arguments_json);
            /* Show a short preview of the tool output. */
            size_t rlen = result ? strlen(result) : 0;
            if (rlen > 0) {
                size_t show = rlen > 600 ? 600 : rlen;
                printf("%s%.*s%s%s\n",
                       C_DIM, (int)show, result,
                       rlen > show ? " …" : "",
                       C_RESET);
            }
            fflush(stdout);

            hist_push_tool_result(h, tc->id, result ? result : "");
            free(result);
        }
    }
    fprintf(stderr, "%s[warn] hit MAX_STEPS without a final answer%s\n", C_YEL, C_RESET);
    return 0;
}

/* ------------------------------------------------------------
 * Line input
 * ------------------------------------------------------------ */
static char *read_line(const char *prompt) {
    fputs(prompt, stdout);
    fflush(stdout);

    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* ------------------------------------------------------------
 * Signal handling: Ctrl-C cancels the current read, not the process
 * ------------------------------------------------------------ */
static volatile sig_atomic_t interrupted = 0;
static void on_sigint(int sig) { (void)sig; interrupted = 1; }

/* ------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------ */
typedef struct {
    const char *endpoint;
    const char *model;
    const char *api_key;
    const char *system;
    int verbose;
} opts_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --endpoint URL   Chat completions server (default: $CODER_ENDPOINT or http://sushi:8000)\n"
        "  --model NAME     Model id (default: $CODER_MODEL or google/gemma-4-31B-it)\n"
        "  --api-key KEY    Optional bearer token (default: $CODER_API_KEY)\n"
        "  --system TEXT    Override system prompt\n"
        "  --verbose, -v    Dump request/response JSON to stderr\n"
        "  --help, -h       Show this help\n", prog);
}

static void parse_args(int argc, char **argv, opts_t *o) {
    o->endpoint = getenv("CODER_ENDPOINT"); if (!o->endpoint) o->endpoint = "http://sushi:8000";
    o->model    = getenv("CODER_MODEL");    if (!o->model)    o->model    = "google/gemma-4-31B-it";
    o->api_key  = getenv("CODER_API_KEY");
    o->system   = NULL;
    o->verbose  = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--endpoint") == 0 && i + 1 < argc) { o->endpoint = argv[++i]; continue; }
        if (strcmp(a, "--model") == 0 && i + 1 < argc)    { o->model    = argv[++i]; continue; }
        if (strcmp(a, "--api-key") == 0 && i + 1 < argc)  { o->api_key  = argv[++i]; continue; }
        if (strcmp(a, "--system") == 0 && i + 1 < argc)   { o->system   = argv[++i]; continue; }
        if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) { o->verbose = 1; continue; }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(argv[0]); exit(0); }
        fprintf(stderr, "unknown arg: %s\n", a); usage(argv[0]); exit(2);
    }
}

/* ------------------------------------------------------------
 * main
 * ------------------------------------------------------------ */
int main(int argc, char **argv) {
    opts_t opts;
    parse_args(argc, argv, &opts);
    use_colour = isatty(fileno(stdout));

    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    llm_client_t client = {
        .endpoint = opts.endpoint,
        .model = opts.model,
        .api_key = opts.api_key,
        .verbose = opts.verbose,
    };

    history_t h; hist_init(&h);
    char *sys_prompt = build_system_prompt(opts.system);
    hist_push_system(&h, sys_prompt);
    free(sys_prompt);

    printf("%scoder%s — %s%s%s via %s%s%s\n",
           C_BOLD, C_RESET,
           C_GREEN, client.model, C_RESET,
           C_BLUE, client.endpoint, C_RESET);
    printf("%stype /quit to exit, /reset to clear history%s\n", C_DIM, C_RESET);

    char prompt[32];
    snprintf(prompt, sizeof prompt, "%s> %s", C_MAG, C_RESET);

    for (;;) {
        interrupted = 0;
        char *line = read_line(prompt);
        if (!line) { printf("\n"); break; }
        if (line[0] == '\0') { free(line); continue; }
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            free(line); break;
        }
        if (strcmp(line, "/reset") == 0) {
            hist_reset(&h);
            printf("%s[history reset]%s\n", C_DIM, C_RESET);
            free(line);
            continue;
        }

        hist_push_user(&h, line);
        free(line);

        run_turn(&client, &h);
    }

    hist_free(&h);
    return 0;
}
