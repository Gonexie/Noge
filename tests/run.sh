#!/bin/sh
# usage: tests/run.sh path/to/nogec
#
#   ok/*.nge    compile, build as warning-free C99, and print exactly ok/*.out
#   trap/*.nge  compile and build, then die with a runtime check: exit status 101 and a stderr line
#               "<file>:LINE: runtime error: MESSAGE". Line 1 is "// trap: LINE: MESSAGE"; trap/*.out
#               is what stdout held up to that point.
#   err/*.nge   must be rejected by nogec; line 1 is "// error: <text the message must contain>", and an
#               optional line 2 "// flags: <nogec options>"
#   ffi/*.nge   like ok/, linked with ffi/<name>.c (C code the program calls, and that calls it back)
#   free/*.nge  built with --freestanding as a -ffreestanding object that must not need stdio or exit
#
# ok/, trap/ and ffi/ run twice: with __builtin_*_overflow, and with --no-builtins (portable C99 checks).
# If clang is installed, ok/ and trap/ are also compiled with it as a second opinion.
NOGEC=${1:-build/nogec}
CC=${CC:-cc}
tmp=$(mktemp -d)
pass=0
fail=0

failed() { echo "FAIL $1"; shift; for x in "$@"; do [ -f "$x" ] && cat "$x"; done; fail=$((fail + 1)); }

# build <flags> <source> <name>: compile .nge -> C -> executable
build() {
    "$NOGEC" $1 -o "$tmp/$3.c" "$2" 2>"$tmp/err" || { failed "$2 [$1]: nogec rejected it" "$tmp/err"; return 1; }
    $CC -std=c99 -Wall -Wextra -Wpedantic -Werror -o "$tmp/$3" "$tmp/$3.c" 2>"$tmp/err" \
        || { failed "$2 [$1]: emitted C does not compile cleanly" "$tmp/err"; return 1; }
}

for mode in "" "--no-builtins"; do
    for f in tests/ok/*.nge; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .nge)
        build "$mode" "$f" "$b" || continue
        "$tmp/$b" >"$tmp/$b.out" 2>"$tmp/$b.stderr"
        if diff -u "${f%.nge}.out" "$tmp/$b.out" >"$tmp/diff"; then
            pass=$((pass + 1))
        else
            failed "$f [$mode]: wrong output" "$tmp/diff"
        fi
    done

    for f in tests/trap/*.nge; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .nge)
        want=$(sed -n '1s|^// trap: ||p' "$f")
        line=${want%%:*}
        msg=${want#*: }
        build "$mode" "$f" "$b" || continue
        "$tmp/$b" >"$tmp/$b.out" 2>"$tmp/$b.stderr"
        code=$?
        if [ "$code" -ne 101 ]; then
            failed "$f [$mode]: expected exit status 101, got $code" "$tmp/$b.stderr"
        elif ! grep -qF -- ":$line: runtime error: $msg" "$tmp/$b.stderr"; then
            failed "$f [$mode]: wanted ':$line: runtime error: $msg'" "$tmp/$b.stderr"
        elif ! diff -u "${f%.nge}.out" "$tmp/$b.out" >"$tmp/diff"; then
            failed "$f [$mode]: wrong output before the trap" "$tmp/diff"
        else
            pass=$((pass + 1))
        fi
    done

    for f in tests/ffi/*.nge; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .nge)
        "$NOGEC" $mode -o "$tmp/$b.c" "$f" 2>"$tmp/err" || { failed "$f [$mode]: nogec rejected it" "$tmp/err"; continue; }
        if ! $CC -std=c99 -Wall -Wextra -Wpedantic -Werror -o "$tmp/$b" "$tmp/$b.c" "${f%.nge}.c" 2>"$tmp/err"; then
            failed "$f [$mode]: does not build with its C support file" "$tmp/err"
            continue
        fi
        "$tmp/$b" >"$tmp/$b.out" 2>"$tmp/$b.stderr"
        if diff -u "${f%.nge}.out" "$tmp/$b.out" >"$tmp/diff"; then pass=$((pass + 1)); else failed "$f [$mode]: wrong output" "$tmp/diff"; fi
    done
done

for f in tests/free/*.nge; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .nge)
    "$NOGEC" --freestanding -o "$tmp/$b.c" "$f" 2>"$tmp/err" || { failed "$f: nogec rejected it" "$tmp/err"; continue; }
    if ! $CC -std=c99 -ffreestanding -O2 -Wall -Wextra -Wpedantic -Werror -c -o "$tmp/$b.o" "$tmp/$b.c" 2>"$tmp/err"; then
        failed "$f: freestanding C does not compile cleanly" "$tmp/err"
        continue
    fi
    if nm -u "$tmp/$b.o" | grep -E 'printf|puts|exit|abort|fflush|stdout|stderr|malloc|free' >"$tmp/diff"; then
        failed "$f: the freestanding object needs the C library" "$tmp/diff"
    else
        pass=$((pass + 1))
    fi
done

# A second opinion: where clang is installed, the emitted C must be warning-free there too (compile only).
if command -v clang >/dev/null 2>&1; then
    for f in tests/ok/*.nge tests/trap/*.nge; do
        [ -e "$f" ] || continue
        b=$(basename "$f" .nge)
        "$NOGEC" -o "$tmp/$b.clang.c" "$f" 2>"$tmp/err" || continue
        if clang -std=c99 -Wall -Wextra -Wpedantic -Werror -c -o "$tmp/$b.clang.o" "$tmp/$b.clang.c" 2>"$tmp/err"; then
            pass=$((pass + 1))
        else
            failed "$f: clang does not accept the emitted C cleanly" "$tmp/err"
        fi
    done
fi

for f in tests/err/*.nge; do
    [ -e "$f" ] || continue
    want=$(sed -n '1s|^// error: ||p' "$f")
    flags=$(sed -n '2s|^// flags: ||p' "$f")
    if "$NOGEC" $flags -o /dev/null "$f" 2>"$tmp/err"; then
        failed "$f: compiled, but should have been rejected"
    elif grep -qF -- "$want" "$tmp/err"; then
        pass=$((pass + 1))
    else
        failed "$f: wrong error, wanted '$want'" "$tmp/err"
    fi
done

rm -rf "$tmp"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
