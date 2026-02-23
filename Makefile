# SwarmRT Makefile

CC = cc
CFLAGS = -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Wno-macro-redefined
LDFLAGS = -pthread

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
EXAMPLES_DIR = examples

# Core object files (needed by all native targets since process_exit hooks into all modules)
CORE_SRCS = swarmrt_native swarmrt_asm swarmrt_otp swarmrt_task swarmrt_ets \
            swarmrt_phase4 swarmrt_phase5 swarmrt_hotload swarmrt_io swarmrt_gc swarmrt_node \
            swarmrt_lang
CORE_OBJS = $(patsubst %,$(BUILD_DIR)/%.o,$(CORE_SRCS))

# Main targets
.PHONY: all clean v1 v2 proc native otp-test phase2 phase3 phase4 phase5 phase6 phase7 phase8 phase9 \
        test test-v1 test-v2 test-proc test-native test-otp test-phase2 test-phase3 test-phase4 \
        test-phase5 test-phase6 test-phase7 test-phase8 test-phase9 test-all benchmark benchmark-native stats \
        swc libswarmrt example-counter

all: v1 v2 proc native

# Compile rules for core objects
$(BUILD_DIR)/swarmrt_asm.o: $(SRC_DIR)/swarmrt_asm.S | dirs
	$(CC) $(CFLAGS) -c $< -o $@

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

test-all: test-v1 test-v2 test-proc test-native

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

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

stats:
	@echo "=== SwarmRT Code Stats ==="
	@find $(SRC_DIR) -name "*.c" -o -name "*.h" | xargs wc -l
	@echo ""
	@echo "=== Example Programs ==="
	@find $(EXAMPLES_DIR) -name "*.sw" | xargs wc -l 2>/dev/null || echo "  (none)"
