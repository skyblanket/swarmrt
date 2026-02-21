# SwarmRT Makefile v2 - Full M:N Threading

CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -pthread
LDFLAGS = -pthread

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
EXAMPLES_DIR = examples

# Main targets
.PHONY: all clean v1 v2 test test-v1 test-v2 benchmark stats

all: v1 v2

v1: dirs
	$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_simple.c -o $(BUILD_DIR)/swarmrt_simple.o
	$(CC) $(CFLAGS) -c $(SRC_DIR)/parser.c -o $(BUILD_DIR)/parser.o
	$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_simple.o $(BUILD_DIR)/parser.o $(SRC_DIR)/main.c -o $(BIN_DIR)/swarmrt-v1 $(LDFLAGS)

v2: dirs
	$(CC) $(CFLAGS) -c $(SRC_DIR)/swarmrt_v2.c -o $(BUILD_DIR)/swarmrt_v2.o
	$(CC) $(CFLAGS) $(BUILD_DIR)/swarmrt_v2.o $(SRC_DIR)/test_v2.c -o $(BIN_DIR)/swarmrt-v2 $(LDFLAGS)

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

test-v1: v1
	./$(BIN_DIR)/swarmrt-v1 test

test-v2: v2
	./$(BIN_DIR)/swarmrt-v2

test: test-v1 test-v2

benchmark: v1
	$(CC) $(CFLAGS) $(SRC_DIR)/benchmark.c $(BUILD_DIR)/swarmrt_simple.o $(BUILD_DIR)/parser.o -o $(BIN_DIR)/benchmark $(LDFLAGS)
	./$(BIN_DIR)/benchmark

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
	@find $(EXAMPLES_DIR) -name "*.sw" | xargs wc -l