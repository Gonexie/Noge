#!/usr/bin/env python3
"""Tests of separate compilation: nogec --emit-dir=DIR writes noge_types.h, one C file per module and a Makefile.

1. Meaning. Every positive test, every multi-file test and every positive test cut into two files (see
   modules_check.py) is built that way with make (-Wall -Wextra -Wpedantic -Werror), run, and must print what the
   one-file build prints, in both overflow modes.
2. Incremental builds. On a three-module project, edits are made and the files nogec writes are compared before and
   after: nothing changed leaves every file alone; a function body, a new function, a new instance of a generic
   function each change only the files that must change; the names of other modules' instances do not change.
3. Freestanding. The C files of a multi-module program compile with -ffreestanding and need nothing from the C library.

usage: tests/separate_check.py [path/to/nogec]        (also: make check-separate)
"""
import glob, hashlib, os, re, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modules_check

NOGEC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/nogec")
CC = os.environ.get("CC", "cc")
STD = os.path.abspath("std")
ENV = dict(os.environ, NOGE_STD=STD)
WARN = "-std=c99 -Wall -Wextra -Wpedantic -Werror"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, env=ENV, **kw)


def build_separately(main, flags, out):
    os.makedirs(out, exist_ok=True)
    r = run([NOGEC] + flags + ["--emit-dir=" + out, main])
    if r.returncode != 0:
        return None, "nogec rejected it:\n" + r.stderr
    m = run(["make", "-s", "-C", out, "CC=" + CC, "CFLAGS=" + WARN + " -O1"])
    if m.returncode != 0:
        return None, "the C does not build:\n" + m.stderr
    prog = [f for f in glob.glob(os.path.join(out, "*")) if os.access(f, os.X_OK) and not f.endswith((".c", ".h", ".o")) and os.path.isfile(f)]
    return (prog[0] if prog else None), None


def check_program(name, main, want_out, want_trap=None):
    for flags in ([], ["--no-builtins"]):
        with tempfile.TemporaryDirectory() as d:
            exe, err = build_separately(main, flags, os.path.join(d, "out"))
            if err:
                return "%s %s: %s" % (name, flags, err)
            e = subprocess.run([exe], capture_output=True, text=True)
            if want_trap is not None:
                if e.returncode != 101 or want_trap not in e.stderr:
                    return "%s %s: wanted a failed check (101) saying '%s', got %d and %r" % (name, flags, want_trap, e.returncode, e.stderr)
            elif e.returncode != 0:
                return "%s %s: the program failed with %d: %s" % (name, flags, e.returncode, e.stderr)
            if e.stdout != want_out:
                return "%s %s: different output:\n  got    %s\n  wanted %s" % (name, flags, e.stdout.split(), want_out.split())
    return None


def meaning_jobs():
    jobs = []
    for f in sorted(glob.glob("tests/ok/*.nge")):
        jobs.append(("ok/" + os.path.basename(f), lambda f=f: check_program(f, f, open(f[:-4] + ".out").read())))
    for d in sorted(glob.glob("tests/mod/*/")):
        trap = open(d + "main.trap").read() if os.path.exists(d + "main.trap") else None
        jobs.append(("mod/" + os.path.basename(d[:-1]), lambda d=d, trap=trap: check_program(d, d + "main.nge", open(d + "main.out").read(), trap)))
    for f in sorted(glob.glob("tests/ok/*.nge")):
        def split_job(f=f):
            lib, main = modules_check.split(open(f).read())
            with tempfile.TemporaryDirectory() as d:
                open(os.path.join(d, "lib.nge"), "w").write(lib)
                open(os.path.join(d, "main.nge"), "w").write(main)
                return check_program("split " + f, os.path.join(d, "main.nge"), open(f[:-4] + ".out").read())
        jobs.append(("split/" + os.path.basename(f), split_job))
    return jobs


# ---- incremental builds
LIB = """pub copy struct Wrapper<T> { v: T }
pub fn wrap<T>(x: T) -> Wrapper<T> { return Wrapper { v: x }; }
pub fn unwrap<T>(w: Wrapper<T>) -> T { return w.v; }
pub fn double(x: i32) -> i32 { return x * 2; }
"""
OTHER = """pub fn triple(x: i32) -> i32 { return x * 3; }
"""
MAIN = """import "lib.nge" as lib;
import "other.nge" as other;
fn main() -> i32 {
    print(lib::unwrap(lib::wrap(5)));
    print(lib::double(4));
    print(other::triple(2));
    return 0;
}
"""


def snapshot(out):
    """per file: what it holds and when it was last written, so that rewriting the same text counts as a write"""
    snap = {}
    for f in sorted(glob.glob(os.path.join(out, "*"))):
        if f.endswith((".c", ".h")) or os.path.basename(f) == "Makefile":
            snap[os.path.basename(f)] = (hashlib.sha1(open(f, "rb").read()).hexdigest(), os.stat(f).st_mtime_ns)
    return snap


def changed(a, b):
    return sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))


def compiles(out, cflags="-O1"):
    """build with make and say how many files it compiled"""
    m = run(["make", "-C", out, "CC=" + CC, "CFLAGS=" + cflags])
    if m.returncode != 0:
        return -1
    return sum(1 for l in m.stdout.split("\n") if " -c " in l)


def idents(path):
    return set(re.findall(r"\bf[0-9a-f]+_\w+|\bfm[0-9a-f]{6}_\w+|\bs[0-9a-f]+_\w+|\be[0-9a-f]+_\w+", open(path).read()))


def incremental():
    problems = []
    with tempfile.TemporaryDirectory() as d:
        write = lambda name, text: open(os.path.join(d, name), "w").write(text)
        write("lib.nge", LIB); write("other.nge", OTHER); write("main.nge", MAIN)
        out = os.path.join(d, "out")
        os.makedirs(out)
        gen = lambda: run([NOGEC, "--emit-dir=" + out, os.path.join(d, "main.nge")])
        r = gen()
        if r.returncode != 0:
            return ["the project does not compile: " + r.stderr]
        base = snapshot(out)
        files = [k for k in base if k.endswith(".c")]
        lib_c = [k for k in files if k.startswith("lib_")][0]
        other_c = [k for k in files if k.startswith("other_")][0]
        n = compiles(out)
        if n != 3:
            problems.append("the first build compiled %d files, wanted 3" % n)

        def expect(what, want, compiled=None):
            nonlocal base
            time.sleep(0.02)
            r = gen()
            if r.returncode != 0:
                problems.append("%s: nogec failed: %s" % (what, r.stderr))
                return
            now = snapshot(out)
            got = changed(base, now)
            base = now
            if got != sorted(want):
                problems.append("%s: files written %s, wanted %s" % (what, got, sorted(want)))
            c = compiles(out)
            if compiled is not None and c != compiled:
                problems.append("%s: make compiled %d files, wanted %d" % (what, c, compiled))

        expect("nothing changed", [], 0)
        write("lib.nge", LIB + "// a comment at the end changes no line of code\n")
        expect("a comment after the last line", [], 0)
        write("lib.nge", LIB.replace("x * 2", "x * 7"))
        expect("a function body in lib", [lib_c], 1)
        write("lib.nge", LIB.replace("x * 2", "x * 7") + "pub fn quad(x: i32) -> i32 { return x * 4; }\n")
        expect("a new function in lib", [lib_c], 1)
        write("other.nge", OTHER.replace("x * 3", "x * 9"))
        expect("a function body in other", [other_c], 1)
        # main asks, before anything else, for a new instance of lib's generic function: it is written into lib's file,
        # and the instances lib already had keep their names although this one now comes first
        before = idents(os.path.join(out, lib_c))
        write("main.nge", MAIN.replace("fn main() -> i32 {\n", "fn main() -> i32 {\n    print(lib::unwrap(lib::wrap(true)) as i32);\n"))
        expect("main uses a new instance of lib's generic function", [lib_c, "main.c", "noge_types.h"], 3)
        after = idents(os.path.join(out, lib_c))
        if not before <= after:
            problems.append("the names of existing instances changed when another was added first: %s" % sorted(before - after))
        # a new type changes the header, and with it everything that includes it
        write("lib.nge", LIB.replace("x * 2", "x * 7") + "pub fn quad(x: i32) -> i32 { return x * 4; }\npub struct Extra { a: i32 }\n")
        expect("a new struct in lib", ["noge_types.h"], 3)
        c = compiles(out, "-O2")
        if c != 3:
            problems.append("other flags: make compiled %d files, wanted 3" % c)
        c = compiles(out, "-O2")
        if c != 0:
            problems.append("the same flags again: make compiled %d files, wanted 0" % c)

    # the C names do not depend on where nogec is run from or how the files are named on the command line
    with tempfile.TemporaryDirectory() as d:
        for n, t in (("lib.nge", LIB), ("other.nge", OTHER), ("main.nge", MAIN)):
            open(os.path.join(d, n), "w").write(t)
        o1, o2 = os.path.join(d, "o1"), os.path.join(d, "o2")
        os.makedirs(o1); os.makedirs(o2)
        r1 = subprocess.run([NOGEC, "--emit-dir=" + o1, "main.nge"], capture_output=True, text=True, cwd=d, env=ENV)
        r2 = subprocess.run([NOGEC, "--emit-dir=" + o2, os.path.join(d, "main.nge")], capture_output=True, text=True, env=ENV)
        if r1.returncode or r2.returncode:
            problems.append("could not build from two places: %s %s" % (r1.stderr, r2.stderr))
        else:
            f1 = sorted(os.path.basename(f) for f in glob.glob(o1 + "/*.c"))
            f2 = sorted(os.path.basename(f) for f in glob.glob(o2 + "/*.c"))
            if f1 != f2:
                problems.append("the files are named differently from two places: %s %s" % (f1, f2))
            else:
                for f in f1:
                    if idents(os.path.join(o1, f)) != idents(os.path.join(o2, f)):
                        problems.append("%s: the C names differ between two places to run nogec from" % f)
    return problems


def freestanding():
    with tempfile.TemporaryDirectory() as d:
        shutil_copy = lambda s, t: open(t, "w").write(open(s).read())
        for n in ("main.nge", "geom.nge", "util.nge"):
            shutil_copy("tests/mod/basic/" + n, os.path.join(d, n))
        out = os.path.join(d, "out")
        os.makedirs(out)
        r = run([NOGEC, "--freestanding", "--emit-dir=" + out, os.path.join(d, "main.nge")])
        # print is not available freestanding: the program uses it, so this is rejected; use one that does not
        if r.returncode == 0:
            return ["a freestanding build of a program that prints was accepted"]
        open(os.path.join(d, "main.nge"), "w").write('import "lib.nge" as lib;\nfn main() -> i32 { return lib::f(3); }\n')
        open(os.path.join(d, "lib.nge"), "w").write("pub fn f(x: i32) -> i32 {\n    let a = [1, 2, 3];\n    return g(x) + a[x as usize % 3];\n}\nfn g(x: i32) -> i32 { return x * 2; }\n")
        r = run([NOGEC, "--freestanding", "--emit-dir=" + out, os.path.join(d, "main.nge")])
        if r.returncode != 0:
            return ["freestanding emit-dir failed: " + r.stderr]
        problems = []
        for c in glob.glob(os.path.join(out, "*.c")):
            o = c[:-2] + ".o"
            b = run([CC, "-std=c99", "-ffreestanding", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", "-o", o, c])
            if b.returncode != 0:
                problems.append("%s: %s" % (os.path.basename(c), b.stderr))
                continue
            nm = run(["nm", "-u", o])
            bad = [l for l in nm.stdout.split("\n") if re.search(r"printf|puts|exit|abort|fflush|stdout|stderr|malloc|free", l)]
            if bad:
                problems.append("%s needs the C library: %s" % (os.path.basename(c), bad))
        return problems


def main():
    failures = 0
    jobs = meaning_jobs()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for (name, _), res in zip(jobs, pool.map(lambda j: j[1](), jobs)):
            if res:
                failures += 1
                if failures <= 5:
                    print("FAIL", res)
    print("%d programs built from separate C files: %s" % (len(jobs), "all print what the one-file build prints" if not failures else "%d FAILED" % failures))
    inc = incremental()
    for p in inc:
        print("FAIL incremental:", p)
    print("incremental builds: %s" % ("only the files that must change are written" if not inc else "%d FAILED" % len(inc)))
    fr = freestanding()
    for p in fr:
        print("FAIL freestanding:", p)
    print("freestanding: %s" % ("the C files compile and need no C library" if not fr else "%d FAILED" % len(fr)))
    sys.exit(1 if failures or inc or fr else 0)


if __name__ == "__main__":
    main()
