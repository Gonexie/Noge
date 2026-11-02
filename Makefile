CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
SRC      = src/util.c src/lexer.c src/parser.c src/check.c src/flow.c src/lower.c src/emit_c.c src/modules.c src/main.c

build/nogec: $(SRC) src/noge.h src/ir.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: build/nogec
	@sh tests/run.sh build/nogec
	@$(MAKE) --no-print-directory check-std
	@$(MAKE) --no-print-directory test-target

# Firmware for a Cortex-M3, run under QEMU. Skipped (with a note) when the toolchain is not installed.
test-target: build/nogec
	@if command -v arm-none-eabi-gcc >/dev/null 2>&1 && command -v qemu-system-arm >/dev/null 2>&1; then \
	    $(MAKE) --no-print-directory -C examples/cortex-m test; \
	else echo "(skipping the Cortex-M tests: arm-none-eabi-gcc and qemu-system-arm are not installed)"; fi

# Slower, independent check of the overflow / division / shift code against __int128 arithmetic.
check-arith: build/nogec
	python3 tests/arith_check.py build/nogec

# Random enums and match arms, checked against a brute-force model and a reference interpreter.
check-match: build/nogec
	python3 tests/match_check.py build/nogec

# The same programs built from separate C files (nogec --emit-dir), and what an incremental build rewrites.
check-separate: build/nogec
	python3 tests/separate_check.py build/nogec

# Every positive test, cut into two files, against its expected output.
check-modules: build/nogec
	python3 tests/modules_check.py build/nogec

# Random generic programs, against the same programs with every instance written out by hand.
check-generics: build/nogec
	python3 tests/generics_check.py build/nogec

# The standard library against independent models written in Python: exhaustive over every 8-bit value where that is
# possible, random operation sequences for the containers, zlib for the checksums, the PCG reference for rand.
check-std: build/nogec
	python3 tests/std_check.py build/nogec

# Noge against Rust: run time of eight programs, and build time of a generated program of 4,000 functions. Needs rustc, gcc,
# clang and tcc, and takes about 20 minutes; leave the machine alone while it runs (see bench/README.md).
bench: build/nogec
	python3 bench/run.py --opt 2 --runs 7
	python3 bench/compile_bench.py --functions 4000 --modules 20 --runs 3

# Damaged programs must be rejected with a message, never crash the compiler. Build nogec with
# -fsanitize=address,undefined (see the top of tests/fuzz.py) to make memory errors visible too.
fuzz: build/nogec
	python3 tests/fuzz.py build/nogec --rounds 5

clean:
	rm -rf build

.PHONY: test test-target check-arith check-match check-generics check-modules check-separate check-std bench fuzz clean
