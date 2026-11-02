#!/usr/bin/env python3
"""Throw damaged programs at the compiler: it may accept them or reject them with a message, but it must
never crash, hang, or print anything but "file:LINE: error: ..." when it fails.

Every program under tests/ is damaged in a few random ways (a token deleted, repeated, swapped with its
neighbour or replaced by another token of the language; or the file cut short) and compiled. Build the
compiler with -fsanitize=address,undefined to make memory errors visible:

    cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o /tmp/nogec-asan src/*.c
    ASAN_OPTIONS=detect_leaks=0 python3 tests/fuzz.py /tmp/nogec-asan --rounds 3

usage: tests/fuzz.py [path/to/nogec] [--rounds N] [--seed S]        (also: make fuzz)
"""
import glob, os, random, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

args = list(sys.argv[1:])
def option(name, default):
    if name in args:
        i = args.index(name)
        value = int(args[i + 1])
        del args[i:i + 2]
        return value
    return default
ROUNDS = option("--rounds", 2)
SEED = option("--seed", 1)
NOGEC = args[0] if args else "build/nogec"

TOKEN = re.compile(r'//[^\n]*|\x27(?:[^\x27\\\n]|\\.[0-9A-Fa-f]*)\x27|c?"(?:[^"\\\n]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|0[xXbB][0-9A-Fa-f_]+|[0-9][0-9_]*|=>|::|->|<<|>>|<=|>=|==|!=|&&|\|\||\S')
KEEP = ("import", "pub", "as", "std", "\"lib.nge\"", "fn", "struct", "enum", "match", "let", "var", "while", "if", "else", "return", "copy", "sink", "inout", "set", "unsafe",
        "extern", "export", "const", "true", "false", "as", "_", "|", "=>", "::", "(", ")", "{", "}", "[", "]", ",", ";", ":", "=",
        "+", "-", "*", "/", "<", ">", "&", "0", "1", "255", "i32", "u8", "bool", "x", "Shape", "Circle")


def damage(rng, text):
    toks = [(m.group(), m.start(), m.end()) for m in TOKEN.finditer(text)]
    toks = [t for t in toks if not t[0].startswith("//")]
    if not toks:
        return text
    for _ in range(rng.randint(1, 3)):
        how = rng.randrange(5)
        k = rng.randrange(len(toks))
        tok, a, b = toks[k]
        if how == 0:
            text = text[:a] + text[b:]
        elif how == 1:
            text = text[:b] + " " + tok + text[b:]
        elif how == 2 and k + 1 < len(toks):
            t2, a2, b2 = toks[k + 1]
            text = text[:a] + t2 + text[b:a2] + tok + text[b2:]
        elif how == 3:
            text = text[:a] + rng.choice(KEEP) + text[b:]
        else:
            text = text[:rng.randrange(len(text) + 1)]
        toks = [(m.group(), m.start(), m.end()) for m in TOKEN.finditer(text)]
        toks = [t for t in toks if not t[0].startswith("//")]
        if not toks:
            break
    return text


def one(job):
    path, n = job
    rng = random.Random(SEED * 7919 + n)
    src = damage(rng, open(path).read())
    with tempfile.TemporaryDirectory() as d:
        here = os.path.dirname(path)
        if os.path.basename(os.path.dirname(here)) in ("mod", "moderr"):  # a program of several files: bring the others
            for other in glob.glob(os.path.join(here, "*.nge")):
                open(os.path.join(d, os.path.basename(other)), "w").write(open(other).read())
        f = os.path.join(d, "f.nge")
        open(f, "w").write(src)
        for flags in ([], ["--no-builtins"]):
            try:
                r = subprocess.run([NOGEC] + flags + ["-o", os.path.join(d, "f.c"), f], capture_output=True, text=True, timeout=20)
            except subprocess.TimeoutExpired:
                return (path, src, "timed out")
            if r.returncode == 0:
                continue
            first = r.stderr.strip().split("\n")[0] if r.stderr.strip() else ""
            if r.returncode != 1:
                return (path, src, "exit status %d\n%s" % (r.returncode, r.stderr[:1500]))
            if not re.match(r"^[^\n]*:\d+: error: ", first) and not first.startswith("nogec: ") and not re.match(r"^[^\n]*: error: ", first):
                return (path, src, "odd message: %s" % first)
            if "internal error" in r.stderr:
                return (path, src, "internal error: %s" % first)
    return None


def main():
    files = sorted(glob.glob("tests/*/*.nge") + glob.glob("tests/mod/*/*.nge") + glob.glob("tests/moderr/*/*.nge"))
    jobs = [(f, i * len(files) + j) for i in range(ROUNDS) for j, f in enumerate(files)]
    bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for res in pool.map(one, jobs):
            if res:
                bad += 1
                if bad <= 5:
                    print("FAIL (from %s): %s\n--- damaged program ---\n%s" % res)
    print("%d damaged programs: %s" % (len(jobs), "none crashed the compiler" if not bad else "%d PROBLEMS" % bad))
    sys.exit(1 if bad else 0)


main()
