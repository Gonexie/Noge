#!/bin/sh
# usage: tests/run.sh path/to/nogec
# ok/*.nge   must compile, build as warning-free C99, and print exactly ok/*.out
# err/*.nge  must be rejected; line 1 is "// error: <text the message must contain>"
NOGEC=${1:-build/nogec}
CC=${CC:-cc}
tmp=$(mktemp -d)
pass=0
fail=0

for f in tests/ok/*.nge; do
    b=$(basename "$f" .nge)
    if ! "$NOGEC" -o "$tmp/$b.c" "$f" 2>"$tmp/err"; then
        echo "FAIL $f: nogec rejected it"; cat "$tmp/err"; fail=$((fail + 1)); continue
    fi
    if ! $CC -std=c99 -Wall -Wextra -Wpedantic -Werror -o "$tmp/$b" "$tmp/$b.c" 2>"$tmp/err"; then
        echo "FAIL $f: emitted C does not compile cleanly"; cat "$tmp/err"; fail=$((fail + 1)); continue
    fi
    "$tmp/$b" >"$tmp/$b.out"
    if diff -u "${f%.nge}.out" "$tmp/$b.out" >"$tmp/diff"; then
        pass=$((pass + 1))
    else
        echo "FAIL $f: wrong output"; cat "$tmp/diff"; fail=$((fail + 1))
    fi
done

for f in tests/err/*.nge; do
    want=$(sed -n '1s|^// error: ||p' "$f")
    if "$NOGEC" -o /dev/null "$f" 2>"$tmp/err"; then
        echo "FAIL $f: compiled, but should have been rejected"; fail=$((fail + 1))
    elif grep -qF -- "$want" "$tmp/err"; then
        pass=$((pass + 1))
    else
        echo "FAIL $f: wrong error, wanted '$want'"; cat "$tmp/err"; fail=$((fail + 1))
    fi
done

rm -rf "$tmp"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
