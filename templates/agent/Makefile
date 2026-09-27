# {{name}} — built with SwarmRT (https://github.com/skyblanket/swarmrt)
SWC ?= swc

.PHONY: build test run clean
build:
	@mkdir -p bin
	$(SWC) build main.sw -o bin/{{name}}

test:
	@mkdir -p bin
	$(SWC) build agent_test.sw -o bin/agent_test
	./bin/agent_test

run: build
	./bin/{{name}}

clean:
	rm -rf bin
