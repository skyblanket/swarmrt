# coder — standalone CLI living inside the swarmrt repo.
# Pure libc, no swarmrt runtime deps (v1). Links nothing but pthread.

CC       ?= cc
CFLAGS   ?= -Wall -Wextra -Wno-unused-function -O2 -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS  ?=

BUILD_DIR := build
BIN       := ../bin/coder

SRCS := coder.c llm.c tools.c
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean
all: $(BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	@mkdir -p $(dir $(BIN))
	$(CC) $(OBJS) -o $(BIN) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) $(BIN)
