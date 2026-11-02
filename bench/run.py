#!/usr/bin/env python3
"""Run-time comparison of Noge and Rust on the same algorithms.

Every program in bench/programs/ exists as NAME.nge and NAME.rs (the same algorithm written the same way, with
while loops and explicit indexing), and some also as NAME_idiomatic.rs (what a Rust programmer would write: iterators,
slices, for loops, which let the compiler drop bounds checks). Each is built several ways:

    noge+gcc    nogec -> C -> gcc -O2          (overflow and bounds checks are in the C that nogec writes)
    noge+clang  nogec -> C -> clang -O2        (clang is LLVM, like rustc: this separates the language from the back end)
    rust        rustc -C opt-level=2 -C overflow-checks=on -C panic=abort -C codegen-units=1
    rust-idiom  the same flags, on the idiomatic version

Arithmetic overflow and array index checks are therefore ON in every build. Nothing is compiled for the machine it runs
on (no -march=native / target-cpu). A time is only reported if every build of a program printed the same output.

usage: bench/run.py [--opt 2|3] [--runs N] [--only a,b] [--keep DIR]
"""
import os, re, resource, shutil, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROGRAMS = os.path.join(HERE, "programs")
NOGEC = os.path.join(ROOT, "build", "nogec")

args = sys.argv[1:]
def option(name, default):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default
OPT = option("--opt", "2")
RUNS = int(option("--runs", "5"))
ONLY = option("--only", "")
KEEP = option("--keep", "")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(name, work):
    """Build every variant of one program; returns {variant: path or None}, and a list of problems."""
    out, problems = {}, []
    nge = os.path.join(PROGRAMS, name + ".nge")
    c = os.path.join(work, name + ".c")
    r = run([NOGEC, "-o", c, nge])
    if r.returncode:
        problems.append("nogec: " + r.stderr[:300])
    else:
        for tag, cc in (("noge+gcc", ["gcc"]), ("noge+clang", ["clang"])):
            exe = os.path.join(work, "%s.%s" % (name, tag))
            r = run(cc + ["-std=c99", "-O" + OPT, "-o", exe, c])
            if r.returncode:
                problems.append("%s: %s" % (tag, r.stderr[:300]))
            else:
                out[tag] = exe
    for tag, src in (("rust", name + ".rs"), ("rust-idiom", name + "_idiomatic.rs")):
        path = os.path.join(PROGRAMS, src)
        if not os.path.exists(path):
            continue
        exe = os.path.join(work, "%s.%s" % (name, tag))
        r = run(["rustc", "-C", "opt-level=" + OPT, "-C", "overflow-checks=on", "-C", "panic=abort",
                 "-C", "codegen-units=1", "-o", exe, path])
        if r.returncode:
            problems.append("%s: %s" % (tag, r.stderr[:300]))
        else:
            out[tag] = exe
    return out, problems


def big_stack():
    resource.setrlimit(resource.RLIMIT_STACK, (1 << 30, 1 << 30))  # the programs keep arrays of up to 20 MB on the stack


def time_one(exe):
    t0 = time.perf_counter()
    r = subprocess.run([exe], capture_output=True, text=True, preexec_fn=big_stack)
    return time.perf_counter() - t0, r


def stripped_size(exe, work):
    s = exe + ".stripped"
    shutil.copy(exe, s)
    subprocess.run(["strip", s], capture_output=True)
    return os.path.getsize(s)


def main():
    names = sorted(f[:-4] for f in os.listdir(PROGRAMS) if f.endswith(".nge"))
    if ONLY:
        names = [n for n in names if n in ONLY.split(",")]
    work = KEEP or tempfile.mkdtemp(prefix="noge-bench-")
    os.makedirs(work, exist_ok=True)
    cols = ["noge+gcc", "noge+clang", "rust", "rust-idiom"]
    rows, bad = [], False
    for name in names:
        exes, problems = build(name, work)
        if problems:
            print("%s: BUILD PROBLEM: %s" % (name, "; ".join(problems)))
            bad = True
            continue
        outputs = {}
        times = {}
        for tag, exe in exes.items():
            samples = []
            for _ in range(RUNS):
                dt, r = time_one(exe)
                if r.returncode != 0:
                    print("%s [%s]: exited with status %d: %s" % (name, tag, r.returncode, r.stderr[:200]))
                    bad = True
                outputs[tag] = r.stdout
                samples.append(dt)
            times[tag] = (min(samples), statistics.median(samples))
        if len(set(outputs.values())) != 1:
            print("%s: OUTPUTS DIFFER: %s" % (name, {k: v.split() for k, v in outputs.items()}))
            bad = True
            continue
        sizes = {tag: stripped_size(exe, work) for tag, exe in exes.items()}
        rows.append((name, times, sizes, outputs[next(iter(outputs))].split()))
    if not rows:
        sys.exit(1)

    print("\nBest of %d runs, seconds (median in brackets); opt level %s; every program printed the same output in every build.\n" % (RUNS, OPT))
    print("| program | " + " | ".join(cols) + " | noge+gcc / rust | noge+clang / rust |")
    print("|---|" + "---:|" * (len(cols) + 2))
    ratios_gcc, ratios_clang = [], []
    for name, times, sizes, out in rows:
        cells = []
        for c in cols:
            cells.append("%.3f (%.3f)" % times[c] if c in times else "")
        rg = times["noge+gcc"][0] / times["rust"][0]
        rc = times["noge+clang"][0] / times["rust"][0]
        ratios_gcc.append(rg)
        ratios_clang.append(rc)
        print("| %s | %s | %.2f | %.2f |" % (name, " | ".join(cells), rg, rc))
    gm = lambda xs: statistics.geometric_mean(xs)
    print("| **geometric mean** | | | | | **%.2f** | **%.2f** |" % (gm(ratios_gcc), gm(ratios_clang)))
    print("\nStripped executable size, bytes:\n")
    print("| program | " + " | ".join(cols) + " |")
    print("|---|" + "---:|" * len(cols))
    for name, times, sizes, out in rows:
        print("| %s | %s |" % (name, " | ".join(str(sizes.get(c, "")) for c in cols)))
    if not KEEP:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if bad else 0)


main()
