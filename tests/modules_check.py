#!/usr/bin/env python3
"""Differential test of modules: a program split into files must behave like the one file.

Every program in tests/ok is cut in two: `fn main` stays in main.nge, which imports lib.nge, and every other item
(structs, enums, constants, functions, generic ones too) moves to lib.nge marked `pub`. Both files are compiled
(with -Wall -Wextra -Wpedantic -Werror) and the program must print what tests/ok/<name>.out says. Everything the
program does across the boundary (calling, naming types, instantiating generic code that uses names only the library
can see) goes through the module machinery, against results that were fixed before modules existed.

usage: tests/modules_check.py [path/to/nogec]        (also: make check-modules)
"""
import glob, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

NOGEC = sys.argv[1] if len(sys.argv) > 1 else "build/nogec"
CC = os.environ.get("CC", "cc").split()
STARTS = re.compile(r"^(pub\s+)?(fn|struct|enum|copy|const|static|extern|export)\b")


def items(text):
    """the top-level items of a program, each with the comment lines in front of it"""
    out, cur, depth, pending = [], None, 0, []
    for line in text.split("\n"):
        code = re.sub(r"//.*", "", re.sub(r'c?"(?:[^"\\]|\\.)*"', '""', line))
        if cur is None:
            if STARTS.match(line):
                cur = pending + [line]
                pending = []
            else:
                pending.append(line)
                continue
        else:
            cur.append(line)
        depth += code.count("{") - code.count("}")
        if depth == 0 and (code.rstrip().endswith("}") or code.rstrip().endswith(";")):
            out.append("\n".join(cur))
            cur = None
    return out


def split(text):
    lib, main = [], []
    for it in items(text):
        body = [l for l in it.split("\n") if STARTS.match(l)][0]
        if re.match(r"fn\s+main\b", body):
            main.append(it)
        else:
            lines = it.split("\n")
            i = [k for k, l in enumerate(lines) if STARTS.match(l)][0]
            lines[i] = "pub " + lines[i]
            lib.append("\n".join(lines))
    return "\n\n".join(lib) + "\n", 'import "lib.nge";\n\n' + "\n\n".join(main) + "\n"


def one(path):
    name = os.path.basename(path)[:-4]
    lib, main = split(open(path).read())
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "lib.nge"), "w").write(lib)
        open(os.path.join(d, "main.nge"), "w").write(main)
        for flags in ([], ["--no-builtins"]):
            r = subprocess.run([NOGEC] + flags + ["-o", os.path.join(d, "p.c"), os.path.join(d, "main.nge")], capture_output=True, text=True)
            if r.returncode != 0:
                return name, "nogec rejected the split program:\n" + r.stderr, lib, main
            c = subprocess.run(CC + ["-std=c99", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-o", os.path.join(d, "p"), os.path.join(d, "p.c")],
                               capture_output=True, text=True)
            if c.returncode != 0:
                return name, "the C does not build:\n" + c.stderr, lib, main
            e = subprocess.run([os.path.join(d, "p")], capture_output=True, text=True)
            want = open(path[:-4] + ".out").read()
            if e.stdout != want:
                return name, "different output:\n  got    %s\n  wanted %s" % (e.stdout.split(), want.split()), lib, main
    return None


def main():
    files = sorted(glob.glob("tests/ok/*.nge"))
    bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for res in pool.map(one, files):
            if res:
                bad += 1
                if bad <= 3:
                    print("FAIL %s: %s\n--- lib.nge ---\n%s\n--- main.nge ---\n%s" % res)
    print("%d programs split into two files: %s" % (len(files), "all print what the single file does" if not bad else "%d FAILED" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
