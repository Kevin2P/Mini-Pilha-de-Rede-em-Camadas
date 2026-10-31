CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
CFLAGS += -Iinclude
SRC = src/common.c src/link.c src/network.c src/transport.c src/application.c
BIN = bin/ministack
TEST_BIN = bin/test_codec

.PHONY: all test clean sanitize fuzz

all: $(BIN) $(TEST_BIN)

$(BIN): $(SRC) src/node.c include/*.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) src/node.c

$(TEST_BIN): $(SRC) tests/test_codec.c include/*.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) tests/test_codec.c

test: all
	./$(TEST_BIN)
	python3 scripts/run_tests.py

sanitize:
	@mkdir -p bin-asan
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -o bin-asan/ministack $(SRC) src/node.c
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -o bin-asan/test_codec $(SRC) tests/test_codec.c
	./bin-asan/test_codec
	STACK_BIN=bin-asan/ministack python3 scripts/run_tests.py

fuzz:
	@mkdir -p bin-asan
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Iinclude -o bin-asan/ministack $(SRC) src/node.c
	STACK_BIN=bin-asan/ministack python3 scripts/fuzz_protocol.py 20000 1

clean:
	rm -rf bin bin-asan
