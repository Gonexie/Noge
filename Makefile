CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
SRC      = src/util.c src/lexer.c src/parser.c src/check.c src/flow.c src/lower.c src/emit_c.c src/main.c

build/nogec: $(SRC) src/noge.h src/ir.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: build/nogec
	@sh tests/run.sh build/nogec

# Slower, independent check of the overflow / division / shift code against __int128 arithmetic.
check-arith: build/nogec
	python3 tests/arith_check.py build/nogec

clean:
	rm -rf build

.PHONY: test check-arith clean
