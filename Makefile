# chatd - TCP chat server
#
#   make          build build/chatd
#   make test     unit tests + integration tests
#   make asan     build and test with AddressSanitizer + UBSan
#   make valgrind full-lifecycle leak check
#   make clean

CC      := gcc
CSTD    := -std=c11 -D_GNU_SOURCE
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wconversion
CFLAGS  := $(CSTD) $(WARN) -O2 -g
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g

SRC     := $(wildcard src/*.c)
LIB     := src/buffer.c src/client.c
BUILD   := build

.PHONY: all test unit integration asan valgrind clean

all: $(BUILD)/chatd

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/chatd: $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SRC) -o $@

$(BUILD)/unit_tests: tests/unit_tests.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(SAN) tests/unit_tests.c $(LIB) -o $@

$(BUILD)/chatd-asan: $(SRC) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(SAN) $(SRC) -o $@

unit: $(BUILD)/unit_tests
	@echo "=== unit tests (ASan + UBSan) ==="
	@./$(BUILD)/unit_tests

integration: $(BUILD)/chatd
	@echo "=== integration tests (real sockets) ==="
	@python3 tests/integration_test.py ./$(BUILD)/chatd

test: unit integration

# The same integration suite, but against a sanitised server: catches
# use-after-free and buffer overflows that the assertions alone would miss.
asan: $(BUILD)/unit_tests $(BUILD)/chatd-asan
	@echo "=== unit tests (ASan + UBSan) ==="
	@./$(BUILD)/unit_tests
	@echo "=== integration tests against a sanitised server ==="
	@ASAN_OPTIONS=detect_leaks=1 python3 tests/integration_test.py ./$(BUILD)/chatd-asan

valgrind: $(BUILD)/chatd
	@./tests/leak_check.sh

clean:
	rm -rf $(BUILD) /tmp/chatd-valgrind.log
