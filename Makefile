CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O1 -g
SRC      = src/util.c src/lexer.c src/parser.c src/check.c src/flow.c src/emit_c.c src/main.c

build/nogec: $(SRC) src/noge.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: build/nogec
	@sh tests/run.sh build/nogec

clean:
	rm -rf build

.PHONY: test clean
