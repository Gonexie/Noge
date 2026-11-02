#!/usr/bin/env python3
"""Compile-time comparison of Noge and Rust on one generated program written in both languages.

The program is N functions spread over M modules. Each function has a loop, a struct, an enum with a `match`, an if/else
and a call to the function before it (the first function of a module calls the last of the module before), so that
nothing can be dropped as dead and the compiler has real work. main calls a few of them and prints one number; the
Noge and the Rust program must print the same, which is checked first.

Noge is built several ways: nogec -> one C file -> gcc/clang/tcc; and nogec --emit-dir (one C file per module) with make -j.
Rust is built with rustc as one crate (modules inline in one file), at opt-level 0 and 2, with the default 16 codegen
units and with 1, and with -C incremental. Overflow checks are on in every Rust build, as they are in Noge's C.

An edit is a change to the body of one function in a module in the middle; "rebuild" is the time to get a new executable.

usage: bench/compile_bench.py [--functions N] [--modules M] [--runs R] [--keep DIR]
"""
import os, resource, shutil, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NOGEC = os.path.join(ROOT, "build", "nogec")
JOBS = str(os.cpu_count() or 2)

args = sys.argv[1:]
def option(name, default):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default
NFUNCS = int(option("--functions", "1000"))
NMODS = int(option("--modules", "10"))
RUNS = int(option("--runs", "3"))
KEEP = option("--keep", "")


def gen_noge(d, tweak=None):
    """tweak = (module, index): the multiplier of that function is 37 instead of 31"""
    per = NFUNCS // NMODS
    with open(os.path.join(d, "common.nge"), "w") as f:
        f.write("""pub copy struct P { a: u32, b: u32 }
pub copy enum K { Low, Mid(u32), High(u32, u32) }
pub fn classify(v: u32) -> K {
    if v < 1000 { return K::Low; }
    if v < 100000 { return K::Mid(v >> 4); }
    return K::High(v >> 16, v & 65535);
}
""")
    for j in range(NMODS):
        out = ['import "common.nge" as c;']
        if j > 0:
            out.append('import "m%d.nge" as prev;' % (j - 1))
        for k in range(per):
            if k > 0:
                callee = "f%d_%d" % (j, k - 1)
            elif j > 0:
                callee = "prev::f%d_%d" % (j - 1, per - 1)
            else:
                callee = None
            mul = 37 if tweak == (j, k) else 31
            call = "%s(a & 1023, b & 1023)" % callee if callee else "(a ^ b)"
            out.append("""
pub fn f%d_%d(x: u32, y: u32) -> u32 {
    var a: u32 = x;
    var b: u32 = y;
    var i: u32 = 0;
    while i < 3 {
        a = wrapping_add(wrapping_mul(a, %d), b ^ i);
        b = wrapping_add(b >> 3, a & 255);
        i = i + 1;
    }
    let p = c::P { a: a, b: b };
    var r: u32 = 0;
    match c::classify(p.a) {
        c::K::Low => { r = p.b; }
        c::K::Mid(m) => { r = wrapping_add(m, p.b); }
        c::K::High(h, l) => { r = wrapping_add(h ^ l, p.a); }
    }
    if p.a > p.b { r = wrapping_add(r, p.a - p.b); } else { r = wrapping_add(r, p.b - p.a); }
    return wrapping_add(r, %s);
}
""" % (j, k, mul, call))
        with open(os.path.join(d, "m%d.nge" % j), "w") as f:
            f.write("\n".join(out))
    picks = [(NMODS - 1, per - 1), (NMODS // 2, per // 2), (0, per - 1), (NMODS - 1, 0)]
    body = ['import "m%d.nge" as m%d;' % (j, j) for j in sorted({p[0] for p in picks})]
    body.append("fn main() -> i32 {\n    var total: u32 = 0;")
    for j, k in picks:
        body.append("    total = wrapping_add(total, m%d::f%d_%d(12345, 678));" % (j, j, k))
    body.append("    print(total as i32);\n    return 0;\n}")
    with open(os.path.join(d, "main.nge"), "w") as f:
        f.write("\n".join(body) + "\n")


def gen_rust(d, tweak=None):
    per = NFUNCS // NMODS
    out = ["""#![allow(dead_code)]
mod common {
    #[derive(Clone, Copy)]
    pub struct P { pub a: u32, pub b: u32 }
    #[derive(Clone, Copy)]
    pub enum K { Low, Mid(u32), High(u32, u32) }
    pub fn classify(v: u32) -> K {
        if v < 1000 { return K::Low; }
        if v < 100000 { return K::Mid(v >> 4); }
        K::High(v >> 16, v & 65535)
    }
}
"""]
    for j in range(NMODS):
        out.append("mod m%d {\n    use super::common as c;" % j)
        for k in range(per):
            if k > 0:
                callee = "f%d_%d" % (j, k - 1)
            elif j > 0:
                callee = "super::m%d::f%d_%d" % (j - 1, j - 1, per - 1)
            else:
                callee = None
            mul = 37 if tweak == (j, k) else 31
            call = "%s(a & 1023, b & 1023)" % callee if callee else "(a ^ b)"
            out.append("""    pub fn f%d_%d(x: u32, y: u32) -> u32 {
        let mut a: u32 = x;
        let mut b: u32 = y;
        let mut i: u32 = 0;
        while i < 3 {
            a = a.wrapping_mul(%d).wrapping_add(b ^ i);
            b = (b >> 3).wrapping_add(a & 255);
            i = i + 1;
        }
        let p = c::P { a: a, b: b };
        let mut r: u32 = 0;
        match c::classify(p.a) {
            c::K::Low => { r = p.b; }
            c::K::Mid(m) => { r = m.wrapping_add(p.b); }
            c::K::High(h, l) => { r = (h ^ l).wrapping_add(p.a); }
        }
        if p.a > p.b { r = r.wrapping_add(p.a - p.b); } else { r = r.wrapping_add(p.b - p.a); }
        return r.wrapping_add(%s);
    }
""" % (j, k, mul, call))
        out.append("}")
    picks = [(NMODS - 1, per - 1), (NMODS // 2, per // 2), (0, per - 1), (NMODS - 1, 0)]
    out.append("fn main() {\n    let mut total: u32 = 0;")
    for j, k in picks:
        out.append("    total = total.wrapping_add(m%d::f%d_%d(12345, 678));" % (j, j, k))
    out.append("    println!(\"{}\", total as i32);\n}")
    with open(os.path.join(d, "big.rs"), "w") as f:
        f.write("\n".join(out) + "\n")


def measure(cmd, cwd=None, runs=None, before=None):
    """best wall time of several runs, plus cpu time and peak memory of the best one (from getrusage deltas)"""
    best = None
    for _ in range(runs or RUNS):
        if before:
            before()
        r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        t0 = time.perf_counter()
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        wall = time.perf_counter() - t0
        r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        if p.returncode != 0:
            raise RuntimeError("%s failed:\n%s" % (cmd, (p.stdout + p.stderr)[-1500:]))
        cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
        if best is None or wall < best[0]:
            best = (wall, cpu)
    return best


def peak_rss_mb(cmd, cwd=None):
    """peak resident memory of the biggest process the command starts (measured in a fresh interpreter)"""
    code = ("import resource,subprocess,sys; subprocess.run(sys.argv[1:], cwd=%r, capture_output=True);"
            "print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)" % cwd)
    out = subprocess.run([sys.executable, "-c", code] + cmd, capture_output=True, text=True).stdout.strip()
    return int(out) / 1024.0


def lines(d, ext):
    return sum(open(os.path.join(d, f)).read().count("\n") for f in os.listdir(d) if f.endswith(ext))


def main():
    work = KEEP or tempfile.mkdtemp(prefix="noge-compile-bench-")
    os.makedirs(work, exist_ok=True)
    nd, rd = os.path.join(work, "noge"), os.path.join(work, "rust")
    for d in (nd, rd):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    gen_noge(nd)
    gen_rust(rd)
    print("%d functions in %d modules: %d lines of Noge, %d lines of Rust\n" % (NFUNCS, NMODS, lines(nd, ".nge"), lines(rd, ".rs")))

    # correctness first: both languages must print the same
    c = os.path.join(nd, "all.c")
    subprocess.run([NOGEC, "-o", c, os.path.join(nd, "main.nge")], check=True)
    subprocess.run(["gcc", "-std=c99", "-O2", "-o", os.path.join(nd, "all"), c], check=True)
    subprocess.run(["rustc", "-C", "opt-level=2", "-C", "overflow-checks=on", "-C", "panic=abort", "-o", os.path.join(rd, "big"), os.path.join(rd, "big.rs")], check=True, capture_output=True)
    a = subprocess.run([os.path.join(nd, "all")], capture_output=True, text=True).stdout
    b = subprocess.run([os.path.join(rd, "big")], capture_output=True, text=True).stdout
    if a != b:
        print("OUTPUTS DIFFER: noge %r rust %r" % (a, b))
        sys.exit(1)
    print("both print %s\n" % a.strip())

    rows = []
    def row(label, cmd, cwd=None, before=None, note=""):
        wall, cpu = measure(cmd, cwd, before=before)
        mem = peak_rss_mb(cmd, cwd) if not before else 0.0
        rows.append((label, wall, cpu, mem, note))
        print("  %-44s %7.2f s wall  %7.2f s cpu  %6.0f MB" % (label, wall, cpu, mem), flush=True)

    mainnge = os.path.join(nd, "main.nge")
    print("Noge")
    row("nogec (front end, one C file)", [NOGEC, "-o", c, mainnge])
    row("  + gcc -O0", ["gcc", "-std=c99", "-O0", "-o", os.path.join(nd, "all0"), c])
    row("  + gcc -O2", ["gcc", "-std=c99", "-O2", "-o", os.path.join(nd, "all2"), c])
    row("  + clang -O2", ["clang", "-std=c99", "-O2", "-o", os.path.join(nd, "allc"), c])
    subprocess.run([NOGEC, "--no-builtins", "-o", os.path.join(nd, "all_nb.c"), mainnge], check=True)
    row("  + tcc (needs --no-builtins C)", ["tcc", "-o", os.path.join(nd, "allt"), os.path.join(nd, "all_nb.c")])
    for exe in ("all0", "all2", "allc", "allt"):
        got = subprocess.run([os.path.join(nd, exe)], capture_output=True, text=True).stdout
        if got != a:
            print("OUTPUT OF %s DIFFERS: %r" % (exe, got))
            sys.exit(1)

    sep = os.path.join(nd, "sep")
    def fresh_sep():
        shutil.rmtree(sep, ignore_errors=True)
        os.makedirs(sep)
    def emit_and_make(opt):
        # one command line so that the whole build is one measured thing
        return ["sh", "-c", "%s --emit-dir=%s %s && make -s -j%s -C %s CFLAGS='-std=c99 -O%s'" % (NOGEC, sep, mainnge, JOBS, sep, opt)]
    row("nogec --emit-dir + make -j%s, gcc -O0" % JOBS, emit_and_make("0"), before=fresh_sep)
    row("nogec --emit-dir + make -j%s, gcc -O2" % JOBS, emit_and_make("2"), before=fresh_sep)

    # an edit: the same function body changed in the middle of the program
    tweak = (NMODS // 2, (NFUNCS // NMODS) // 2)
    nd2 = os.path.join(work, "noge_edit")
    shutil.rmtree(nd2, ignore_errors=True)
    os.makedirs(nd2)
    gen_noge(nd2, tweak)
    rd_chk = os.path.join(work, "rust_edit_check")
    shutil.rmtree(rd_chk, ignore_errors=True)
    os.makedirs(rd_chk)
    gen_rust(rd_chk, tweak)
    subprocess.run(["rustc", "-C", "opt-level=0", "-C", "overflow-checks=on", "-C", "panic=abort", "-o", os.path.join(rd_chk, "e"), os.path.join(rd_chk, "big.rs")], check=True, capture_output=True)
    subprocess.run([NOGEC, "-o", os.path.join(nd2, "e.c"), os.path.join(nd2, "main.nge")], check=True)
    subprocess.run(["gcc", "-std=c99", "-O0", "-o", os.path.join(nd2, "e"), os.path.join(nd2, "e.c")], check=True)
    ea = subprocess.run([os.path.join(nd2, "e")], capture_output=True, text=True).stdout
    eb = subprocess.run([os.path.join(rd_chk, "e")], capture_output=True, text=True).stdout
    if ea != eb or ea == a:
        print("EDITED PROGRAMS: noge %r rust %r (unedited %r)" % (ea, eb, a))
        sys.exit(1)
    print("  (the edited program prints %s in both languages)" % ea.strip())
    edited = os.path.join(nd, "m%d.nge" % tweak[0])
    original = open(edited).read()
    def edit_noge():
        shutil.copy(os.path.join(nd2, "m%d.nge" % tweak[0]), edited)
    def unedit_noge():
        open(edited, "w").write(original)
    # warm build, then edit and rebuild
    fresh_sep()
    subprocess.run(["sh", "-c", "%s --emit-dir=%s %s && make -s -j%s -C %s CFLAGS='-std=c99 -O2'" % (NOGEC, sep, mainnge, JOBS, sep)], check=True)
    best = None
    for _ in range(RUNS):
        unedit_noge()
        subprocess.run(["sh", "-c", "%s --emit-dir=%s %s && make -s -j%s -C %s CFLAGS='-std=c99 -O2'" % (NOGEC, sep, mainnge, JOBS, sep)], check=True)
        edit_noge()
        before = {f: os.stat(os.path.join(sep, f)).st_mtime_ns for f in os.listdir(sep) if f.endswith(".o")}
        t0 = time.perf_counter()
        subprocess.run(["sh", "-c", "%s --emit-dir=%s %s && make -s -j%s -C %s CFLAGS='-std=c99 -O2'" % (NOGEC, sep, mainnge, JOBS, sep)], check=True)
        wall = time.perf_counter() - t0
        after = {f: os.stat(os.path.join(sep, f)).st_mtime_ns for f in os.listdir(sep) if f.endswith(".o")}
        rebuilt = sum(1 for f in after if before.get(f) != after[f])
        if best is None or wall < best[0]:
            best = (wall, rebuilt)
    unedit_noge()
    rows.append(("nogec --emit-dir + make, one function edited", best[0], 0, 0, "%d of %d files recompiled" % (best[1], NMODS + 2)))
    print("  %-44s %7.2f s wall  (%d C files recompiled)" % ("one function edited, gcc -O2", best[0], best[1]))

    print("\nRust (one crate)")
    base = ["rustc", "-C", "overflow-checks=on", "-C", "panic=abort", "--edition", "2021"]
    big = os.path.join(rd, "big.rs")
    row("rustc opt-level=0", base + ["-C", "opt-level=0", "-o", os.path.join(rd, "b0"), big])
    row("rustc opt-level=2 (16 codegen units)", base + ["-C", "opt-level=2", "-o", os.path.join(rd, "b2"), big])
    row("rustc opt-level=2, 1 codegen unit", base + ["-C", "opt-level=2", "-C", "codegen-units=1", "-o", os.path.join(rd, "b2s"), big])
    inc = os.path.join(rd, "inc")
    for lvl in ("0", "2"):
        shutil.rmtree(inc, ignore_errors=True)
        cmd = base + ["-C", "opt-level=" + lvl, "-C", "incremental=" + inc, "-o", os.path.join(rd, "bi"), big]
        rd2 = os.path.join(work, "rust_edit")
        shutil.rmtree(rd2, ignore_errors=True)
        os.makedirs(rd2)
        gen_rust(rd2, tweak)
        orig_rs = open(big).read()
        edited_rs = open(os.path.join(rd2, "big.rs")).read()
        subprocess.run(cmd, check=True, capture_output=True)  # cold
        best = None
        for _ in range(RUNS):
            open(big, "w").write(orig_rs)
            subprocess.run(cmd, check=True, capture_output=True)
            open(big, "w").write(edited_rs)
            t0 = time.perf_counter()
            subprocess.run(cmd, check=True, capture_output=True)
            wall = time.perf_counter() - t0
            if best is None or wall < best:
                best = wall
        open(big, "w").write(orig_rs)
        rows.append(("rustc -C incremental, opt-level=%s, one function edited" % lvl, best, 0, 0, ""))
        print("  %-44s %7.2f s wall" % ("one function edited, incremental, opt-level=" + lvl, best), flush=True)

    print("\n| build | wall s | cpu s | peak MB |\n|---|---:|---:|---:|")
    for label, wall, cpu, mem, note in rows:
        print("| %s | %.2f | %s | %s |%s" % (label, wall, "%.2f" % cpu if cpu else "", "%.0f" % mem if mem else "", " " + note if note else ""))
    if not KEEP:
        shutil.rmtree(work, ignore_errors=True)


main()
