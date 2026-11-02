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
#   mod/*/      programs of several files: main.nge (and what it imports) must print main.out; a main.trap holds
#               text that must be on stderr when the run ends in a failed check (status 101): every line of it
#               The mod/ programs are also built as one C file per module (nogec --emit-dir, then make), and must
#               print the same.
#   moderr/*/   main.nge must be rejected, with the text on its first line "// error: <text>" in the message
#
# ok/, trap/ and ffi/ run twice: with __builtin_*_overflow, and with --no-builtins (portable C99 checks).
# If clang is installed, ok/ and trap/ are also compiled with it as a second opinion.
# The one-step runner (noge) is checked too: output, exit status and clean-up.
NOGEC=${1:-build/nogec}
NOGE_STD=$PWD/std # the standard library, wherever nogec was built
export NOGE_STD
CC=${CC:-cc}
tmp=$(mktemp -d)
pass=0
fail=0

# all_lines_in <wanted> <file>: every line of <wanted> is somewhere in <file>
all_lines_in() { while IFS= read -r l || [ -n "$l" ]; do grep -qF -- "$l" "$2" || return 1; done < "$1"; }

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

    for d in tests/mod/*/; do
        [ -e "${d}main.nge" ] || continue
        b=mod_$(basename "$d")
        build "$mode" "${d}main.nge" "$b" || continue
        "$tmp/$b" >"$tmp/$b.out" 2>"$tmp/$b.stderr"
        code=$?
        if [ -f "${d}main.trap" ]; then
            if [ "$code" -ne 101 ]; then
                failed "$d [$mode]: expected exit status 101, got $code" "$tmp/$b.stderr"
            elif ! all_lines_in "${d}main.trap" "$tmp/$b.stderr"; then
                failed "$d [$mode]: wanted '$(cat "${d}main.trap")' on stderr" "$tmp/$b.stderr"
            elif ! diff -u "${d}main.out" "$tmp/$b.out" >"$tmp/diff"; then
                failed "$d [$mode]: wrong output before the failed check" "$tmp/diff"
            else
                pass=$((pass + 1))
            fi
        elif diff -u "${d}main.out" "$tmp/$b.out" >"$tmp/diff"; then
            pass=$((pass + 1))
        else
            failed "$d [$mode]: wrong output" "$tmp/diff"
        fi
    done

    for d in tests/mod/*/; do
        [ -e "${d}main.nge" ] || continue
        b=sep_$(basename "$d")
        mkdir -p "$tmp/$b.dir"
        if ! "$NOGEC" $mode --emit-dir="$tmp/$b.dir" "${d}main.nge" 2>"$tmp/err"; then
            failed "$d [$mode]: nogec rejected it (as separate C files)" "$tmp/err"
            continue
        fi
        if ! make -s -C "$tmp/$b.dir" CC="$CC" CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Werror" >"$tmp/err" 2>&1; then
            failed "$d [$mode]: the separate C files do not build cleanly" "$tmp/err"
            continue
        fi
        "$tmp/$b.dir/main" >"$tmp/$b.out" 2>"$tmp/$b.stderr"
        code=$?
        want_code=0
        [ -f "${d}main.trap" ] && want_code=101
        if [ "$code" -ne "$want_code" ]; then
            failed "$d [$mode]: separate C files: exit status $code, wanted $want_code" "$tmp/$b.stderr"
        elif diff -u "${d}main.out" "$tmp/$b.out" >"$tmp/diff"; then
            pass=$((pass + 1))
        else
            failed "$d [$mode]: separate C files: wrong output" "$tmp/diff"
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

# The one-step runner, ./noge: the right output, the program's own exit status, and no temporary files left.
rt="$tmp/runner-tmp"
mkdir -p "$rt"
runner_check() { # runner_check <name> <wanted status> <wanted stdout> <text wanted on stderr, or empty> <args...>
    name=$1; want_code=$2; want_out=$3; want_err=$4
    shift 4
    got_out=$(TMPDIR="$rt" NOGEC="$NOGEC" sh noge "$@" 2>"$tmp/rerr")
    got_code=$?
    if [ "$got_code" = "$want_code" ] && [ "$got_out" = "$want_out" ] && { [ -z "$want_err" ] || grep -qF -- "$want_err" "$tmp/rerr"; }; then
        pass=$((pass + 1))
    else
        failed "noge ($name): status $got_code (wanted $want_code), output [$got_out]" "$tmp/rerr"
    fi
}
runner_check "gcc" 0 42 "" --gcc tests/ok/hello.nge
runner_check "build-dir" 0 "$(cat tests/mod/basic/main.out)" "" --build-dir="$tmp/bd" tests/mod/basic/main.nge
runner_check "build-dir again" 0 "$(cat tests/mod/basic/main.out)" "" --build-dir="$tmp/bd" tests/mod/basic/main.nge
if command -v tcc >/dev/null 2>&1; then runner_check "tcc" 0 42 "" tests/ok/hello.nge; fi
runner_check "run-time check" 101 1 ":5: runtime error: integer overflow" tests/trap/add_i32.nge
runner_check "compile error" 1 "" "use of moved value" tests/err/move_then_use.nge
runner_check "missing file" 2 "" "no such file" tests/ok/nope.nge
if [ "$(ls -d "$rt"/noge.* 2>/dev/null | wc -l)" -eq 0 ]; then
    pass=$((pass + 1))
else
    failed "noge left temporary folders behind: $(ls "$rt")"
fi

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

for d in tests/moderr/*/; do
    [ -e "${d}main.nge" ] || continue
    want=$(sed -n '1s|^// error: ||p' "${d}main.nge")
    if "$NOGEC" -o /dev/null "${d}main.nge" 2>"$tmp/err"; then
        failed "$d: compiled, but should have been rejected"
    elif grep -qF -- "$want" "$tmp/err"; then
        pass=$((pass + 1))
    else
        failed "$d: wrong error, wanted '$want'" "$tmp/err"
    fi
done

# the notes after an error inside nested generic instances come innermost first: the std function that failed, then
# the std function that asked for it, then the line of the program
"$NOGEC" -o /dev/null tests/moderr/std_nested_note_names_the_user_line/main.nge 2>"$tmp/err"
if sed -n 2p "$tmp/err" | grep -q "std/vec.nge" && sed -n 3p "$tmp/err" | grep -q "std_nested_note_names_the_user_line/main.nge" && [ "$(wc -l <"$tmp/err")" -eq 3 ]; then
    pass=$((pass + 1))
else
    failed "the notes of a nested instance are not innermost first, one line each" "$tmp/err"
fi

# ... and an instance built inside another one is named once, not once for each way of finding out about it
"$NOGEC" -o /dev/null tests/moderr/generic_nested_type_note_inner/main.nge 2>"$tmp/err"
if sed -n 2p "$tmp/err" | grep -q "'Inner<0>'" && sed -n 3p "$tmp/err" | grep -q "'Outer<0>'" && [ "$(wc -l <"$tmp/err")" -eq 3 ]; then
    pass=$((pass + 1))
else
    failed "the notes of a nested type instance are wrong" "$tmp/err"
fi

"$NOGEC" -o /dev/null tests/moderr/generic_nested_type_in_fn_note/main.nge 2>"$tmp/err"
if sed -n 2p "$tmp/err" | grep -q "'Inner<0>'" && sed -n 3p "$tmp/err" | grep -q "'Outer<0>'" && sed -n 4p "$tmp/err" | grep -q "'make<0>'" && [ "$(wc -l <"$tmp/err")" -eq 4 ]; then
    pass=$((pass + 1))
else
    failed "the notes of a type built inside a generic function are wrong" "$tmp/err"
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
