# SwarmRT Makefile

CC = cc
# Per-source files use `#ifndef _GNU_SOURCE / #define _GNU_SOURCE` guards
# so -D_GNU_SOURCE is no longer needed on the command line. Kept the
# define on Darwin only because some macOS-specific APIs (kqueue, etc.)
# still want _DARWIN_C_SOURCE visible early. The Linux build uses neither.
CFLAGS = -Wall -Wextra -Wno-unused-function -g -O2 -pthread
ifeq ($(shell uname),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
endif

# `-Wno-macro-redefined` is clang-only. gcc accepts it silently on the
# command line but then *notes* "unrecognized command-line option" when
# it later emits any other diagnostic — which makes every build look
# noisy. Just check whether the compiler is clang and only pass the
# flag in that case.
IS_CLANG := $(shell $(CC) --version 2>/dev/null | head -1 | grep -qi clang && echo yes || echo no)
ifeq ($(IS_CLANG),yes)
CFLAGS += -Wno-macro-redefined
endif

LDFLAGS = -pthread -lz -lsqlite3
ifneq ($(shell uname),Darwin)
LDFLAGS += -lssl -lcrypto -lm
endif

# On macOS Homebrew installs sqlite headers under /opt/homebrew/opt/sqlite3.
# The system one (in /usr/include) is also valid but Homebrew's is newer.
ifeq ($(shell uname),Darwin)
SQLITE_PREFIX := $(shell brew --prefix sqlite3 2>/dev/null)
ifneq ($(SQLITE_PREFIX),)
CFLAGS += -I$(SQLITE_PREFIX)/include
LDFLAGS += -L$(SQLITE_PREFIX)/lib
endif
endif

# Optional wss:// (WebSocket-over-TLS) for the WS client (wsc_connect_tls).
# Linux already links -lssl/-lcrypto so TLS is on by default there. On macOS
# the build defaults to CommonCrypto and OFF; opt in with `make SWARMRT_TLS=1`
# (requires Homebrew openssl@3). When OFF, wsc_connect_tls still serves ws://
# + custom headers and returns nil for wss:// — the build never breaks.
ifeq ($(shell uname),Darwin)
ifeq ($(SWARMRT_TLS),1)
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(OPENSSL_PREFIX),)
CFLAGS  += -DSWARMRT_TLS -DSWARMRT_OPENSSL_PREFIX='"$(OPENSSL_PREFIX)"' -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif
endif
endif

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
EXAMPLES_DIR = examples

# Core object files (needed by all native targets since process_exit hooks into all modules)
CORE_SRCS = swarmrt_native swarmrt_asm swarmrt_otp swarmrt_task swarmrt_ets \
            swarmrt_phase4 swarmrt_phase5 swarmrt_hotload swarmrt_io swarmrt_gc swarmrt_node \
            swarmrt_lang swarmrt_http swarmrt_pdf swarmrt_varena
CORE_OBJS = $(patsubst %,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# Main targets
.PHONY: all clean v1 v2 proc native otp-test phase2 phase3 phase4 phase5 phase6 phase7 phase8 phase9 phase10 \
        test test-v1 test-v2 test-proc test-native test-otp test-phase2 test-phase3 test-phase4 \
        test-phase5 test-phase6 test-phase7 test-phase8 test-phase9 test-phase10 test-all test-core test-full \
        test-sw benchmark benchmark-native stats h2h swc libswarmrt example-counter search test-search bench-search \
        sws mcp mcp-wrap examples

all: v1 v2 proc native

# Compile rules for core objects
$(BUILD_DIR)/swarmrt_asm.o: $(SRC_DIR)/swarmrt_asm.S | dirs
	$(CC) $(CFLAGS) -c $< -o $@

# swarmrt_native.c needs -fno-stack-protector: processes migrate between
# scheduler threads via context_swap, invalidating the per-thread canary
$(BUILD_DIR)/swarmrt_native.o: $(SRC_DIR)/swarmrt_native.c | dirs
	$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

core-objs: $(CORE_OBJS)

v1: dirs
	$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_simple.c -o $(BUILD_DIR)/swarmrt_simple.o
	$(CC) $(CFLAGS) -c $(SRC_DIR)/parser.c -o $(BUILD_DIR)/parser.o
	$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_simple.o $(BUILD_DIR)/parser.o $(SRC_DIR)/main.c -o $(BIN_DIR)/swarmrt-v1 $(LDFLAGS)

v2: dirs
	$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_v2.c -o $(BUILD_DIR)/swarmrt_v2.o
	$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_v2.o $(SRC_DIR)/test_v2.c -o $(BIN_DIR)/swarmrt-v2 $(LDFLAGS)

# Process subsystem build (with optional assembly context switching)
proc: dirs
	$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_proc.c -o $(BUILD_DIR)/swarmrt_proc.o
	@$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_context.S -o $(BUILD_DIR)/swarmrt_context.o 2>/dev/null || true
	@$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_proc.o $(BUILD_DIR)/swarmrt_context.o $(SRC_DIR)/test_proc.c -o $(BIN_DIR)/swarmrt-proc $(LDFLAGS) 2>/dev/null || \
	$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_proc.o $(SRC_DIR)/test_proc.c -o $(BIN_DIR)/swarmrt-proc $(LDFLAGS)

# Native runtime (benchmark)
native: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/benchmark_native.c -o $(BIN_DIR)/swarmrt-native $(LDFLAGS)

# Behaviour feature tests
otp-test: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_otp.c -o $(BIN_DIR)/test-otp $(LDFLAGS)

# Phase 2 tests (GenServer + Supervisor)
phase2: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase2.c -o $(BIN_DIR)/test-phase2 $(LDFLAGS)

# Phase 3 tests (Task + ETS)
phase3: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase3.c -o $(BIN_DIR)/test-phase3 $(LDFLAGS)

# Phase 4 tests (Agent + Application + DynamicSupervisor)
phase4: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase4.c -o $(BIN_DIR)/test-phase4 $(LDFLAGS)

# Phase 5 tests (GenStateMachine + Process Groups)
phase5: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase5.c -o $(BIN_DIR)/test-phase5 $(LDFLAGS)

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# Test targets
test-v1: v1
	./$(BIN_DIR)/swarmrt-v1 test

test-v2: v2
	./$(BIN_DIR)/swarmrt-v2

test-proc: proc
	./$(BIN_DIR)/swarmrt-proc

test-native: native
	./$(BIN_DIR)/swarmrt-native

test-otp: otp-test
	./$(BIN_DIR)/test-otp

test-phase2: phase2
	./$(BIN_DIR)/test-phase2

test-phase3: phase3
	./$(BIN_DIR)/test-phase3

test-phase4: phase4
	./$(BIN_DIR)/test-phase4

test-phase5: phase5
	./$(BIN_DIR)/test-phase5

# Phase 6 tests (IO/Port system)
phase6: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase6.c -o $(BIN_DIR)/test-phase6 $(LDFLAGS)

test-phase6: phase6
	./$(BIN_DIR)/test-phase6

# Phase 7 tests (Hot Code Reload)
phase7: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase7.c -o $(BIN_DIR)/test-phase7 $(LDFLAGS)

test-phase7: phase7
	./$(BIN_DIR)/test-phase7

# Phase 8 tests (GC & Heap)
phase8: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase8.c -o $(BIN_DIR)/test-phase8 $(LDFLAGS)

test-phase8: phase8
	./$(BIN_DIR)/test-phase8

# Phase 9 tests (Node & Distribution)
phase9: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase9.c -o $(BIN_DIR)/test-phase9 $(LDFLAGS)

test-phase9: phase9
	./$(BIN_DIR)/test-phase9

# Phase 10 tests (Language Frontend)
phase10: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/test_phase10.c -o $(BIN_DIR)/test-phase10 $(LDFLAGS)

test-phase10: phase10
	./$(BIN_DIR)/test-phase10

# Head-to-head benchmark
h2h: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/benchmark_h2h.c -o $(BIN_DIR)/bench-h2h $(LDFLAGS)

test: test-v1 test-v2 test-proc

# Core test gate (what test-all used to be). Kept as a named alias so
# callers can depend on it without hard-coding the list.
test-core: test-v1 test-v2 test-proc test-native test-sw

# Backward-compat alias — contributors/scripts that invoke `make test-all`
# continue to work unchanged.
test-all: test-core

# Full runtime gate: core tests + OTP + all phase tests + search + tools.
# This is the target CI and reviewers should run before claiming green.
test-full: test-core test-otp test-phase2 test-phase3 test-phase4 test-phase5 test-phase6 test-phase7 test-phase8 test-phase9 test-phase10 test-search mcp sws test-injection

# Compile every example that has a fun main() as a quick smoke-test of
# the compiler and core runtime across real programs.
examples: swc libswarmrt
	@for f in $(EXAMPLES_DIR)/*.sw; do \
		if grep -q "^fun main()" "$$f"; then \
			echo "Building $$f..."; \
			./$(BIN_DIR)/swc build "$$f" -o "$(BIN_DIR)/$$(basename $$f .sw)" --emit-c || exit 1; \
		fi; \
	done

# sw-language test suite. Each tests/sw/test_*.sw file is compiled with
# bin/swc, run, and its summary captured. The driver script aggregates
# pass/fail counts across all files. See tests/sw/run_tests.sh.
test-sw: swc libswarmrt
	@./tests/sw/run_tests.sh
	@./tests/sw/run_conform.sh

# Security regression: the curl-backed HTTP builtins must not pass
# caller-supplied URLs / headers through a shell. Builds the injection
# probe and fails if any shell payload executes (canary file appears).
.PHONY: test-injection
test-injection: swc libswarmrt
	@./bin/swc build tests/security/shell_injection_test.sw -o /tmp/sw_injection_test
	@SW_QUIET=1 /tmp/sw_injection_test

# Doc-truth tripwires.
#   check-docs : every complete ```sw block in the docs + every runnable
#                example COMPILES with this swc.
#   doctest    : every complete ```sw block carrying `# =>` expected-output
#                markers is COMPILED, RUN, and its stdout asserted line by
#                line — the "docs lie" guard. Fails non-zero on drift.
.PHONY: check-docs doctest
check-docs: swc libswarmrt
	@bash scripts/check_sw_docs.sh
doctest: swc libswarmrt
	@bash scripts/doctest.sh

# ── Sanitizer / fuzz ─────────────────────────────────────────────────
# Fuzz the two pure, untrusted-input parsers — sw_lang_parse (every .sw
# file) and sw_unmarshal (every byte off a remote node). Both are free of
# the scheduler / asm context-switching, so they're clean under ASAN+UBSAN
# (a full-runtime ASAN build false-positives on the fiber stack swaps).
#
#   make fuzz            — build both standalone + ASAN/UBSAN, replay the
#                          seed corpus + 20k deterministic mutations each.
#                          A crash aborts non-zero (CI memory-safety gate).
#   SW_FUZZ_ITERS=1000000 make fuzz   — longer soak.
#
# For coverage-guided libFuzzer use a clang with the fuzzer runtime
# (Linux clang, or `brew install llvm`): see tests/fuzz/README.md.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-stack-protector -g -O1
FUZZ_CC ?= clang
# Runtime sources the harnesses link against (mirror CORE_SRCS; asm is .S).
FUZZ_RT := $(SRC_DIR)/swarmrt_native.c $(SRC_DIR)/swarmrt_asm.S $(SRC_DIR)/swarmrt_otp.c \
           $(SRC_DIR)/swarmrt_task.c $(SRC_DIR)/swarmrt_ets.c $(SRC_DIR)/swarmrt_phase4.c \
           $(SRC_DIR)/swarmrt_phase5.c $(SRC_DIR)/swarmrt_hotload.c $(SRC_DIR)/swarmrt_io.c \
           $(SRC_DIR)/swarmrt_gc.c $(SRC_DIR)/swarmrt_node.c $(SRC_DIR)/swarmrt_lang.c \
           $(SRC_DIR)/swarmrt_http.c $(SRC_DIR)/swarmrt_pdf.c $(SRC_DIR)/swarmrt_varena.c
ASAN_FUZZ_ENV := ASAN_OPTIONS=detect_leaks=0:abort_on_error=1

.PHONY: fuzz fuzz-parse fuzz-marshal fuzz-http fuzz-json
fuzz-parse: dirs
	$(FUZZ_CC) $(CFLAGS) $(SAN) -DSW_FUZZ_STANDALONE -Itests/fuzz \
	    tests/fuzz/fuzz_parse.c $(FUZZ_RT) -o $(BIN_DIR)/fuzz_parse $(LDFLAGS)
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/fuzz_parse tests/fuzz/corpus/parse

# JSON decoder — the agent-facing input boundary (every LLM tool-call
# response / parsed HTTP body). Proves no crash/over-read/stack-overflow
# on malformed or adversarially-nested input (the corpus includes a
# 5000-deep array; g_jd_depth must bound it).
fuzz-json: dirs
	$(FUZZ_CC) $(CFLAGS) $(SAN) -DSW_FUZZ_STANDALONE -Itests/fuzz -I$(SRC_DIR) \
	    tests/fuzz/fuzz_json.c $(FUZZ_RT) -o $(BIN_DIR)/fuzz_json $(LDFLAGS)
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/fuzz_json tests/fuzz/corpus/json

fuzz-marshal: dirs
	$(FUZZ_CC) $(CFLAGS) $(SAN) -DSW_FUZZ_STANDALONE -Itests/fuzz \
	    tests/fuzz/fuzz_marshal.c $(FUZZ_RT) -o $(BIN_DIR)/fuzz_marshal $(LDFLAGS)
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/fuzz_marshal tests/fuzz/corpus/marshal

# HTTP request-header parser (the headers-to-handler delivery surface).
# -DSW_FUZZ_HTTP exposes sw_http_fuzz_parse() in swarmrt_http.c (built once,
# in FUZZ_RT, with the flag applied to the whole single-command compile).
fuzz-http: dirs
	$(FUZZ_CC) $(CFLAGS) $(SAN) -DSW_FUZZ_STANDALONE -DSW_FUZZ_HTTP -Itests/fuzz \
	    tests/fuzz/fuzz_http.c $(FUZZ_RT) -o $(BIN_DIR)/fuzz_http $(LDFLAGS)
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/fuzz_http tests/fuzz/corpus/http

fuzz: fuzz-parse fuzz-json fuzz-marshal fuzz-http

# GC v1 correctness gate: the copy-on-escape stress harness compiled with ASAN +
# SW_ARENA_POISON. Workers build compound values in their per-process arenas,
# hand them to longer-lived readers (parent / ETS / pmap / task_stream), then
# exit — freeing + poisoning (0xDE) their arena. A missed deep-copy at any escape
# boundary surfaces as an ASAN use-after-free or a 0xDE-garbage content assert.
# Emits the generated C, compiles it + the runtime under the sanitizers, runs it.
.PHONY: gc-stress
gc-stress: swc libswarmrt
	@./bin/swc build --emit-c tests/sw/test_uaf_stress.sw -o $(BIN_DIR)/_gc_stress_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/sw/test_uaf_stress.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_stress $(LDFLAGS)
	@rm -f tests/sw/test_uaf_stress.gen.c $(BIN_DIR)/_gc_stress_emit
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_stress
	@# Cross-fiber adopt-ownership repro: a pmap driver leaking adopt across its
	@# blocking receive must NOT make another fiber's sleep()-receive free an
	@# arena-interior pointer. Single-scheduler forces the deterministic interleave.
	@./bin/swc build --emit-c tests/gc/tls_adopt_repro.sw -o $(BIN_DIR)/_tls_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/tls_adopt_repro.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_tls_repro $(LDFLAGS)
	@rm -f tests/gc/tls_adopt_repro.gen.c $(BIN_DIR)/_tls_emit
	@SW_SCHEDULERS=1 $(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_tls_repro
	@# Cross-process error()/try repro: generated `_sw_error` must be per-process,
	@# not scheduler-thread-local. A process parked in `try { sleep(...); ... }`
	@# must NOT catch an unrelated process's error() — whose value lives in that
	@# process's arena and is freed when it exits (-> heap-UAF in the wrong catch).
	@# Single-scheduler makes the victim-outlives-contaminants interleave deterministic.
	@./bin/swc build --emit-c tests/gc/error_xproc_repro.sw -o $(BIN_DIR)/_xperr_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/error_xproc_repro.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_error_repro $(LDFLAGS)
	@rm -f tests/gc/error_xproc_repro.gen.c $(BIN_DIR)/_xperr_emit
	@SW_SCHEDULERS=1 $(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_error_repro
	@# Cross-process panic-TRACE isolation: generated line/file/call-trace must be
	@# per-process, not scheduler-thread-local. A panic must print the panicking
	@# process's OWN call chain — not frames left by an unrelated fiber parked deep
	@# on the same scheduler thread. Observable only on stderr, so a grep gate.
	@bash scripts/gc_trace_check.sh tests/gc/trace_xproc_repro.sw stress
	@# ETS cross-process aliasing: ets_get must COPY the value out, so a reader
	@# holding a looked-up value is immune to another process replacing/deleting the
	@# key (which now frees the stored copy). A shared pointer would heap-UAF when
	@# the holder deep-reads its value after the churner freed it.
	@./bin/swc build --emit-c tests/gc/ets_alias_repro.sw -o $(BIN_DIR)/_etsa_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/ets_alias_repro.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_ets_alias $(LDFLAGS)
	@rm -f tests/gc/ets_alias_repro.gen.c $(BIN_DIR)/_etsa_emit
	@SW_SCHEDULERS=1 $(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_ets_alias
	@# Supervisor child closures: each child gets a private COPY (freed in its own
	@# process_destroy, crash-safe); the supervisor frees the MASTER at permanent
	@# removal/teardown. Freeing the master must NOT race a live child (kill is
	@# async, the closure is the child's running code) — the copy/master split
	@# avoids that. dynsup_churn = start_child/terminate_child UAF check;
	@# sup_restart = crash+restart+teardown double-free tripwire (master reused on
	@# restart, copy freed per dead incarnation on the panic path).
	@./bin/swc build --emit-c tests/gc/slope_dynsup_churn.sw -o $(BIN_DIR)/_dsc_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/slope_dynsup_churn.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_dynsup_churn $(LDFLAGS)
	@rm -f tests/gc/slope_dynsup_churn.gen.c $(BIN_DIR)/_dsc_emit
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_dynsup_churn 300 >/dev/null
	@./bin/swc build --emit-c tests/gc/sup_restart_repro.sw -o $(BIN_DIR)/_srr_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/sup_restart_repro.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_sup_restart $(LDFLAGS)
	@rm -f tests/gc/sup_restart_repro.gen.c $(BIN_DIR)/_srr_emit
	@# Children panic on purpose here; hide the expected panic banners on success,
	@# but surface stderr (incl. any ASAN report) if it exits non-zero.
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_sup_restart 8 >/dev/null 2>$(BIN_DIR)/_srr.err || (cat $(BIN_DIR)/_srr.err; rm -f $(BIN_DIR)/_srr.err; exit 1)
	@rm -f $(BIN_DIR)/_srr.err
	@# Supervisor KILL-teardown (exit_proc — the only teardown verb sw exposes for a
	@# bare supervisor): a killed supervisor never resumes its fiber, so teardown
	@# runs via the supervisor's own on_destroy hook. Checks the kill-path destructor
	@# frees masters+nodes+st without double-freeing (children freed their copies).
	@./bin/swc build --emit-c tests/gc/slope_sup_kill.sw -o $(BIN_DIR)/_ssk_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/slope_sup_kill.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_sup_kill $(LDFLAGS)
	@rm -f tests/gc/slope_sup_kill.gen.c $(BIN_DIR)/_ssk_emit
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_sup_kill 40 >/dev/null
	@# process_info() reg_entry UAF: _builtin_process_info must read
	@# proc->reg_entry->name UNDER registry.lock — registry_remove_proc free()s
	@# that entry under the wrlock when a registered process exits. 64 named
	@# guards crash-restart (free+realloc of reg_entry) while 8 fibers hammer
	@# process_info() on every pid. Pre-fix: ASan heap-use-after-free in strlen
	@# within ~4s. The introspection builtins (process_info/process_list/
	@# registered) are exercised by no other gc-stress repro, which is exactly
	@# why this UAF shipped. Guards panic on purpose — hide the banners, surface
	@# any ASan report on failure.
	@./bin/swc build --emit-c tests/gc/process_info_uaf_repro.sw -o $(BIN_DIR)/_piu_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) -DSW_ARENA_POISON \
	    tests/gc/process_info_uaf_repro.gen.c $(FUZZ_RT) -o $(BIN_DIR)/gc_process_info_uaf $(LDFLAGS)
	@rm -f tests/gc/process_info_uaf_repro.gen.c $(BIN_DIR)/_piu_emit
	@$(ASAN_FUZZ_ENV) $(BIN_DIR)/gc_process_info_uaf >/dev/null 2>$(BIN_DIR)/_piu.err || (cat $(BIN_DIR)/_piu.err; rm -f $(BIN_DIR)/_piu.err; exit 1)
	@rm -f $(BIN_DIR)/_piu.err

# Mailbox depth-cap gate (SW_MAILBOX_MAX) — bidirectional. Capped run: a 200k
# flood into a parked receiver admits exactly the cap, the rest is dropped
# (counter-exact), the receiver survives, sw_call into a flooded server times
# out instead of hanging, and the EXIT/timer cap exemptions hold. Uncapped
# run (SW_MAILBOX_MAX=0): zero drops and ALL 200k arrive — the same
# assertions fail on a runtime without the cap, so the gate is bidirectional.
# Built under ASAN: also proves the drop path frees RAW payloads and VALUE
# regions (no leak / UAF / double-free on rejection).
.PHONY: mailbox-flood
mailbox-flood: dirs
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) $(SAN) \
	    tests/stress/mailbox_flood.c $(FUZZ_RT) -o $(BIN_DIR)/mailbox_flood $(LDFLAGS)
	@SW_MAILBOX_MAX=1000 $(ASAN_FUZZ_ENV) $(BIN_DIR)/mailbox_flood capped
	@SW_MAILBOX_MAX=0 $(ASAN_FUZZ_ENV) $(BIN_DIR)/mailbox_flood uncapped

# Per-process memory quota gate (SW_PROC_MEM_MAX) — bidirectional. Capped run:
# a hog child accumulating ~6.5 MB of LIVE strings under a 2 MB quota is killed
# by the arena grow/adopt quota check — the stderr panic names SW_PROC_MEM_MAX
# + the pid, the parent sees the monitor DOWN, and a sibling process + the
# parent complete normally (PROBE_OK — the node survives, fault isolation
# holds). Uncapped run: the SAME binary with the quota unset completes the
# full hog with NO quota banner (no false kills). Neuter sw_varena_quota_check
# and the capped run fails — the gate has been proven bidirectionally.
.PHONY: quota-gate
quota-gate: swc libswarmrt
	@./bin/swc build tests/gc/quota_kill.sw -o $(BIN_DIR)/quota_kill >/dev/null
	@SW_PROC_MEM_MAX=2000000 $(BIN_DIR)/quota_kill >$(BIN_DIR)/_qk.out 2>$(BIN_DIR)/_qk.err; rc=$$?; \
	 if [ $$rc -ne 0 ] || ! grep -q "PROBE_OK" $(BIN_DIR)/_qk.out; then \
	   echo "quota-gate: FAIL (capped run rc=$$rc — hog not killed cleanly or node died)"; \
	   cat $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; rm -f $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; exit 1; fi; \
	 if ! grep -q "SW_PROC_MEM_MAX exceeded" $(BIN_DIR)/_qk.err; then \
	   echo "quota-gate: FAIL (no SW_PROC_MEM_MAX panic banner on stderr)"; \
	   cat $(BIN_DIR)/_qk.err; rm -f $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; exit 1; fi
	@$(BIN_DIR)/quota_kill >$(BIN_DIR)/_qk.out 2>$(BIN_DIR)/_qk.err; rc=$$?; \
	 if [ $$rc -ne 0 ] || ! grep -q "PROBE_OK" $(BIN_DIR)/_qk.out; then \
	   echo "quota-gate: FAIL (uncapped run rc=$$rc — hog should complete with no quota)"; \
	   cat $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; rm -f $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; exit 1; fi; \
	 if grep -q "SW_PROC_MEM_MAX exceeded" $(BIN_DIR)/_qk.err; then \
	   echo "quota-gate: FAIL (false quota kill with SW_PROC_MEM_MAX unset)"; \
	   cat $(BIN_DIR)/_qk.err; rm -f $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; exit 1; fi; \
	 rm -f $(BIN_DIR)/_qk.out $(BIN_DIR)/_qk.err; \
	 echo "quota-gate: PASS (bidirectional — hog killed under cap, completes uncapped)"

# GC memory-slope gate (Ownership v2): run the escaped-value slope probes at a low
# and high count (fixed concurrency / mailbox depth / turns) and fail if peak-RSS
# growth exceeds budget. Each probe only counts if it exits 0 AND prints PROBE_OK
# (scripts/gc_slope.sh enforces this — a crashing/sys_exit(1) probe FAILS). Covers
# spawn args, value messages, a long-lived tail loop, pmap, ETS churn (a fixed
# live-set hammered with put-replace/delete/take must hold flat), timer closures
# (delay one-shot + interval cancel), and dynamic-supervisor child churn
# (start_child/terminate_child must free the master closure). Honors
# SW_SCHEDULERS / SW_GC_OFF.
.PHONY: gc-slope
gc-slope: swc
	@rc=0; \
	 bash scripts/gc_slope.sh tests/gc/slope_spawn.sw     200 2000   80 spawn    || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_message.sw   500 4000   80 msg      || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_turn.sw      5000 100000 80 turn     || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_pmap.sw      200 2000   80 pmap     || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_prestart.sw  500 3000   60 prestart || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_ets.sw       2000 20000 10 ets      || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_timer.sw     2000 20000 8  timer    || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_interval.sw  200  2000  10 interval || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_dynsup_churn.sw 2000 20000 8 dynsup || rc=1; \
	 bash scripts/gc_slope.sh tests/gc/slope_sup_kill.sw  200  1500 12 supkill  || rc=1; \
	 [ $$rc -eq 0 ] && echo "gc-slope: PASS (bounded)" || echo "gc-slope: FAIL (unbounded)"; \
	 exit $$rc

# LeakSanitizer lifecycle gate (Phase 2.1) — Linux only (macOS Apple-clang
# ASAN ships no LSan; every other ASAN run here uses detect_leaks=0 and is
# leak-BLIND — see PRODUCTION_ROADMAP.md). Churns every lifecycle owner
# (timers fired+cancelled, supervisors killed, ETS replace/delete, spawns,
# compound messages), exits cleanly, and lets LSan assert zero
# definitely-lost blocks at exit. Parked-fiber stacks can't false-positive
# because at clean exit every process is gone; globals-reachable singletons
# are "still reachable" and not reported. Suppressions (each one a
# documented accepted-minor) live in tests/gc/lsan.supp.
.PHONY: lsan-gate
lsan-gate: swc
	@if [ "$$(uname)" != "Linux" ]; then \
	  echo "lsan-gate: SKIP (LeakSanitizer requires Linux clang/gcc ASAN)"; exit 0; \
	fi
	@./bin/swc build --emit-c tests/gc/lsan_lifecycle.sw -o $(BIN_DIR)/_lsan_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) -fsanitize=address -g -O1 -fno-stack-protector \
	    tests/gc/lsan_lifecycle.gen.c $(FUZZ_RT) -o $(BIN_DIR)/lsan_gate $(LDFLAGS)
	@rm -f tests/gc/lsan_lifecycle.gen.c $(BIN_DIR)/_lsan_emit
	@out=$$(SW_QUIET=1 SW_SCHEDULERS=1 \
	     ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=23 \
	     LSAN_OPTIONS=suppressions=tests/gc/lsan.supp:print_suppressions=1 \
	     timeout 240 $(BIN_DIR)/lsan_gate 2>&1); rc=$$?; \
	 echo "$$out" | tail -25; \
	 if ! echo "$$out" | grep -q "PROBE_OK"; then echo "lsan-gate: FAIL (probe did not complete)"; exit 1; fi; \
	 if [ $$rc -eq 23 ] || echo "$$out" | grep -q "definitely lost\|SUMMARY: AddressSanitizer.*leak"; then \
	   echo "lsan-gate: FAIL (leaks at exit)"; exit 1; fi; \
	 echo "lsan-gate: PASS (zero unsuppressed leaks at exit)"

# Allocation-failure injection gate (Phase 2.5). Builds the workload with
# -DSW_ALLOC_FAULT + ASAN and runs it once per fail-point N (SW_FAIL_ALLOC_AT),
# so the Nth value/region allocation fails exactly once. Each run must exit
# 0 (clean) or 1 (loud OOM/spawn panic) — BOTH fine. The gate FAILS only on
# an ASAN report (use-after-free / double-free / heap overflow): a fault that
# corrupted memory instead of unwinding. Sweeps a range that covers the
# spawn/send/supervise/ETS ownership-transfer sites. Linux/ASAN only.
.PHONY: alloc-fault
alloc-fault: swc
	@if [ "$$(uname)" != "Linux" ]; then echo "alloc-fault: SKIP (needs Linux ASAN)"; exit 0; fi
	@./bin/swc build --emit-c tests/gc/alloc_fault.sw -o $(BIN_DIR)/_af_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) -fsanitize=address -DSW_ALLOC_FAULT -g -O1 -fno-stack-protector \
	    tests/gc/alloc_fault.gen.c $(FUZZ_RT) -o $(BIN_DIR)/alloc_fault $(LDFLAGS)
	@rm -f tests/gc/alloc_fault.gen.c $(BIN_DIR)/_af_emit
	@rc=0; faults=0; clean=0; \
	 for n in $$(seq 1 120); do \
	   out=$$(SW_QUIET=1 SW_SCHEDULERS=1 SW_FAIL_ALLOC_AT=$$n \
	          ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	          timeout 30 $(BIN_DIR)/alloc_fault 2>&1); \
	   ec=$$?; \
	   if echo "$$out" | grep -qE "AddressSanitizer|runtime error:|heap-use-after-free|double-free"; then \
	     echo "alloc-fault: FAIL at SW_FAIL_ALLOC_AT=$$n (memory error)"; \
	     echo "$$out" | grep -A8 "AddressSanitizer" | head -20; rc=1; break; \
	   fi; \
	   if echo "$$out" | grep -q "alloc_fault DONE"; then clean=$$((clean+1)); else faults=$$((faults+1)); fi; \
	 done; \
	 [ $$rc -eq 0 ] && echo "alloc-fault: PASS (120 fail-points: $$clean clean, $$faults loud-OOM, 0 memory errors)" \
	                || exit 1

# Soak (Phase 2.6): mixed-workload (actor fan-out + supervisor crash/restart
# + ETS churn + timers) run for SOAK_SECONDS while sampling RSS. Asserts clean
# exit + peak RSS under budget — the whole mix stays flat under sustained
# churn, not just the per-owner slope probes. CI runs the 60s smoke; the full
# 24h soak is the same binary with SOAK_SECONDS=86400 on a dedicated host.
.PHONY: soak
soak: swc libswarmrt
	@./tests/soak/run_soak.sh

# Stress: 80k-spawn microbench across default scheduler count and
# SW_SCHEDULERS=1. Defaults to 50 runs per variant and requires every
# run to print `ok 80000`. Requires native Linux x86_64 thread scheduling
# for meaningful scheduler-race coverage; emulated/serialised setups
# (Docker on Apple Silicon, valgrind, qemu user-mode) are smoke only.
.PHONY: stress
stress: swc libswarmrt
	@./tests/stress/run_stress.sh

# Benchmarks
benchmark: v1
	$(CC) $(CFLAGS) $(SRC_DIR)/benchmark.c $(BUILD_DIR)/swarmrt_simple.o $(BUILD_DIR)/parser.o -o $(BIN_DIR)/benchmark $(LDFLAGS)
	./$(BIN_DIR)/benchmark

benchmark-native: native
	./$(BIN_DIR)/swarmrt-native

# SwarmRT static library (for linking compiled .sw programs)
libswarmrt: core-objs
	ar rcs $(BIN_DIR)/libswarmrt.a $(CORE_OBJS)

# SwarmRT compiler
swc: core-objs
	$(CC) $(CFLAGS) $(CORE_OBJS) $(SRC_DIR)/swc.c $(SRC_DIR)/swarmrt_codegen.c $(SRC_DIR)/swarmrt_obfusc.c \
		$(SRC_DIR)/swarmrt_repl.c $(SRC_DIR)/swarmrt_test.c $(SRC_DIR)/swarmrt_lsp.c \
		-o $(BIN_DIR)/swc $(LDFLAGS)

# Example: compile a .sw file
example-counter: swc libswarmrt
	./$(BIN_DIR)/swc build $(EXAMPLES_DIR)/counter.sw -o $(BIN_DIR)/counter --emit-c

# Atelier Mally — autonomous creative studio
atelier: swc libswarmrt
	@mkdir -p studio/output
	./$(BIN_DIR)/swc build studio/atelier.sw -o $(BIN_DIR)/atelier --emit-c

parse-test: v1
	./$(BIN_DIR)/swarmrt-v1 parse $(EXAMPLES_DIR)/hello.sw
	./$(BIN_DIR)/swarmrt-v1 parse $(EXAMPLES_DIR)/counter.sw

# Search module (SIMD fuzzy + vector search)
search: dirs
	$(CC) $(CFLAGS) -march=native -c $(SRC_DIR)/swarmrt_search.c -o $(BUILD_DIR)/swarmrt_search.o

test-search: search
	$(CC) $(CFLAGS) -march=native $(BUILD_DIR)/swarmrt_search.o $(SRC_DIR)/test_search.c \
		-o $(BIN_DIR)/test-search $(LDFLAGS) -lm
	./$(BIN_DIR)/test-search

bench-search: search
	$(CC) $(CFLAGS) -march=native $(BUILD_DIR)/swarmrt_search.o $(SRC_DIR)/bench_search.c \
		-o $(BIN_DIR)/bench-search $(LDFLAGS) -lm
	./$(BIN_DIR)/bench-search

# Filesystem search CLI
sws: search dirs
	$(CC) $(CFLAGS) -march=native $(BUILD_DIR)/swarmrt_search.o \
		$(SRC_DIR)/swarmrt_fsindex.c -o $(BIN_DIR)/sws $(LDFLAGS) -lm

# MCP server for Claude Code
mcp: search dirs
	$(CC) $(CFLAGS) -march=native $(BUILD_DIR)/swarmrt_search.o \
		mcp/swarmrt_mcp.c -o $(BIN_DIR)/swarmrt-mcp $(LDFLAGS) -lm

# PTY wrapper that injects scheduled wakes into a Claude Code session.
# On Linux, forkpty() requires -lutil; on macOS it's in libc (libutil is a stub).
WRAP_LDFLAGS = -pthread
ifneq ($(shell uname),Darwin)
WRAP_LDFLAGS += -lutil
endif
mcp-wrap: dirs
	$(CC) $(CFLAGS) mcp/swarmrt_wrap.c -o $(BIN_DIR)/swarmrt-wrap $(WRAP_LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) dist
	# Compiled .sw programs that test/example targets leave at the repo
	# root, plus generated C and debug bundles — build artefacts, not
	# source (see .gitignore).
	rm -rf atelier counter_test error_test* ets_test hello_test hello_test_bin* \
	       import_main integration_test my_test \
	       feedback_test wake_test lab_*
	find . -name '*.gen.c' -delete
	find . -name '*.dSYM' -prune -exec rm -rf {} +

stats:
	@echo "=== SwarmRT Code Stats ==="
	@find $(SRC_DIR) -name "*.c" -o -name "*.h" | xargs wc -l
	@echo ""
	@echo "=== Example Programs ==="
	@find $(EXAMPLES_DIR) -name "*.sw" | xargs wc -l 2>/dev/null || echo "  (none)"

# ThreadSanitizer gate (Phase 2.3). Builds the depth-1 message ping-pong
# and the 80k spawn storm under -fsanitize=thread and fails on any
# unsuppressed race. The fiber runtime is TSan-clean because cross-thread
# fiber migration synchronizes through the runq's C11 atomics — TSan sees
# the happens-before edges; no fiber annotations needed. Suppressions
# (tests/stress/tsan.supp) cover only the documented warn-only watchdog
# scanner. The storm runs its full 80k spawns — ~2 min under TSan.
.PHONY: tsan-gate
tsan-gate: swc
	@./bin/swc build --emit-c tests/gc/slope_message.sw -o $(BIN_DIR)/_tsm_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) -fsanitize=thread -g -O1 \
	    tests/gc/slope_message.gen.c $(FUZZ_RT) -o $(BIN_DIR)/tsan_msg $(LDFLAGS)
	@./bin/swc build --emit-c tests/stress/bn.sw -o $(BIN_DIR)/_tsb_emit >/dev/null 2>&1 || true
	$(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) -fsanitize=thread -g -O1 \
	    tests/stress/bn.gen.c $(FUZZ_RT) -o $(BIN_DIR)/tsan_storm $(LDFLAGS)
	@rm -f tests/gc/slope_message.gen.c tests/stress/bn.gen.c $(BIN_DIR)/_tsm_emit $(BIN_DIR)/_tsb_emit
	@# phase 2 (GenServer/Supervisor), 4 (Agent/DynSup), 5 (StateMachine/PG) are
	@# all TSan-clean and gated here.
	@for p in 2 4 5; do \
	   $(FUZZ_CC) $(CFLAGS) -I$(SRC_DIR) -fsanitize=thread -g -O1 \
	     $(SRC_DIR)/test_phase$$p.c $(FUZZ_RT) -o $(BIN_DIR)/tsan_phase$$p $(LDFLAGS); \
	 done
	@rc=0; \
	 out1=$$(SW_QUIET=1 TSAN_OPTIONS="suppressions=tests/stress/tsan.supp" \
	         timeout 300 $(BIN_DIR)/tsan_msg 4000 2>&1) || rc=1; \
	 echo "$$out1" | grep -q "WARNING: ThreadSanitizer" && { echo "$$out1" | head -40; rc=1; }; \
	 out2=$$(SW_QUIET=1 SW_SPIN_US=30 TSAN_OPTIONS="suppressions=tests/stress/tsan.supp" \
	         timeout 300 $(BIN_DIR)/tsan_msg 4000 2>&1) || rc=1; \
	 echo "$$out2" | grep -q "WARNING: ThreadSanitizer" && { echo "$$out2" | head -40; rc=1; }; \
	 out3=$$(SW_QUIET=1 TSAN_OPTIONS="suppressions=tests/stress/tsan.supp" \
	         timeout 300 $(BIN_DIR)/tsan_storm 2>&1) || rc=1; \
	 echo "$$out3" | grep -q "WARNING: ThreadSanitizer" && { echo "$$out3" | head -40; rc=1; }; \
	 for p in 2 4 5; do \
	   outp=$$(SW_QUIET=1 TSAN_OPTIONS="suppressions=tests/stress/tsan.supp" \
	           timeout 300 $(BIN_DIR)/tsan_phase$$p 2>&1) || rc=1; \
	   echo "$$outp" | grep -q "WARNING: ThreadSanitizer" && { echo "phase$$p:"; echo "$$outp" | grep -A6 WARNING | head -28; rc=1; }; \
	 done; \
	 rm -f $(BIN_DIR)/tsan_phase2 $(BIN_DIR)/tsan_phase4 $(BIN_DIR)/tsan_phase5; \
	 [ $$rc -eq 0 ] && echo "tsan-gate: PASS (no unsuppressed races: msg, msg+spin, storm, phase 2/4/5)" \
	                || { echo "tsan-gate: FAIL"; exit 1; }
