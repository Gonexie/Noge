#!/usr/bin/env python3
"""Check the standard library against independent models written in Python.

Each suite generates a Noge program that exercises one std module (exhaustively over every 8-bit value where it
can, with seeded random inputs otherwise) and prints checksums or results; the expected output is computed here
by a model that shares no code with the library (zlib for CRC-32 and Adler-32, `sorted` for sorting, plain
integers for arithmetic). The program is built with nogec and gcc, in both overflow modes, and its output must
match the model's exactly.

usage: tests/std_check.py [path/to/nogec] [--seed S] [--only suite[,suite]] [--keep DIR]        (also: make check-std)
"""
import os, random, re, subprocess, sys, tempfile, zlib, binascii
from concurrent.futures import ThreadPoolExecutor

args = list(sys.argv[1:])
def option(name, default, conv=str):
    if name in args:
        i = args.index(name)
        value = conv(args[i + 1])
        del args[i:i + 2]
        return value
    return default
SEED = option("--seed", 1, int)
ONLY = option("--only", "")
KEEP = option("--keep", "")
NOGEC = os.path.abspath(args[0] if args else "build/nogec")
STD = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "std"))
CC = os.environ.get("CC", "cc").split()  # a compiler, perhaps with flags: CC="gcc -O2"

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def tdiv(a, b):  # C division: rounds towards zero
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def trem(a, b):
    return a - b * tdiv(a, b)


class Checksum:
    """The model of `acc = wrapping_add(wrapping_mul(acc, 31), v as u32)` in the generated programs."""
    def __init__(self):
        self.acc = 0

    def add(self, v):
        self.acc = (self.acc * 31 + (v & M32)) & M32

    def value(self):
        return s32(self.acc)


def build_and_run(name, files, flags=()):
    """files: {relative name: text}; main.nge is built. Returns stdout, or raises with the details."""
    with tempfile.TemporaryDirectory() as d:
        for rel, text in files.items():
            with open(os.path.join(d, rel), "w") as f:
                f.write(text)
        if KEEP:
            os.makedirs(KEEP, exist_ok=True)
            with open(os.path.join(KEEP, name + ".nge"), "w") as f:
                f.write(files["main.nge"])
        outs = []
        for mode in ([], ["--no-builtins"]):
            c = os.path.join(d, "main.c")
            r = subprocess.run([NOGEC, "--std-dir=" + STD, *mode, *flags, "-o", c, os.path.join(d, "main.nge")], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("nogec rejected it:\n" + r.stderr)
            exe = os.path.join(d, "main")
            r = subprocess.run([*CC, "-std=c99", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-o", exe, c], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError("the emitted C does not build cleanly:\n" + r.stderr[:2000])
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            if r.returncode != 0:
                raise RuntimeError("the program failed (status %d):\n%s" % (r.returncode, r.stderr[:500]))
            outs.append(r.stdout)
        if outs[0] != outs[1]:
            raise RuntimeError("the two overflow modes disagree")
        # the same program as one C file per module must give the same answer
        sep = os.path.join(d, "sep")
        os.makedirs(sep)
        r = subprocess.run([NOGEC, "--std-dir=" + STD, "--emit-dir=" + sep, *flags, os.path.join(d, "main.nge")], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("nogec rejected it as separate files:\n" + r.stderr)
        r = subprocess.run(["make", "-s", "-C", sep, "CC=" + " ".join(CC), "CFLAGS=-std=c99 -O1 -Wall -Wextra -Wpedantic -Werror"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("the separate C files do not build cleanly:\n" + (r.stdout + r.stderr)[:2000])
        r = subprocess.run([os.path.join(sep, "main")], capture_output=True, text=True, timeout=120)
        if r.returncode != 0 or r.stdout != outs[0]:
            raise RuntimeError("as separate C files the program printed something else (status %d)" % r.returncode)
        return outs[0]


# ---------------------------------------------------------------------------------------------------------------
# helpers shared by the suites
# ---------------------------------------------------------------------------------------------------------------
INT_TYPES = {"i8": (True, 8), "u8": (False, 8), "i16": (True, 16), "u16": (False, 16), "i32": (True, 32),
             "u32": (False, 32), "i64": (True, 64), "u64": (False, 64)}


def tbounds(t):
    signed, bits = INT_TYPES[t]
    return (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)


def lit(v):
    """A literal for the value v (negative ones are written with a minus sign)."""
    return str(v)


def table(name, t, values):
    return "static %s: [%s; %d] = [%s];\n" % (name, t, len(values), ", ".join(lit(v) for v in values))


def edge_values(t):
    lo, hi = tbounds(t)
    signed, bits = INT_TYPES[t]
    base = {0, 1, 2, 3, 7, 10, 100, 127, 128, 255, 256, 46340, 46341, 65535, 65536, hi // 3, hi // 2, hi - 1, hi, lo, lo + 1, lo // 2, lo // 3}
    if signed:
        base |= {-1, -2, -3, -10, -127, -128, -129, -255, -46340, -46341, -65535}
    return sorted(v for v in base if lo <= v <= hi)


def value_grid(t):
    lo, hi = tbounds(t)
    signed, bits = INT_TYPES[t]
    if bits == 8:
        return list(range(lo, hi + 1))
    if bits == 16:
        vals = set(range(lo, hi + 1, 257)) | {lo, lo + 1, hi - 1, hi, 0, 1, 2, 3, 255, 256}
        if signed:
            vals |= {-1, -2, -3, -256}
        return sorted(v for v in vals if lo <= v <= hi)
    return edge_values(t)


# ---------------------------------------------------------------------------------------------------------------
# math
# ---------------------------------------------------------------------------------------------------------------
MATH_NAMES = ["min", "max", "abs", "sign", "abs_diff", "div_ceil", "rem_euclid", "align_up", "gcd", "lcm", "pow", "isqrt",
              "cadd", "csub", "cmul", "clamp"]

MATH_HEAD = """import "std/math.nge" as math;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
"""

MATH_GUARD_FUNCS = """fn abs64(x: i64) -> i64 { if x < 0 { return 0 - x; } return x; }
fn gcd64(a: i64, b: i64) -> i64 {
    var x = abs64(a);
    var y = abs64(b);
    while y != 0 { let t = x % y; x = y; y = t; }
    return x;
}
fn ceil_div64(a: i64, b: i64) -> i64 {
    let q = a / b;
    let r = a % b;
    if r != 0 && ((r < 0) == (b < 0)) { return q + 1; }
    return q;
}
fn pow64(b: i64, e: u32) -> i64 {
    var r: i64 = 1;
    var i: u32 = 0;
    while i < e { r = r * b; i = i + 1; }
    return r;
}
fn fits(v: i64) -> bool { return v >= @TMIN@ && v <= @TMAX@; }
"""


def math_model(t, vals, guarded):
    lo, hi = tbounds(t)
    fits = lambda v: lo <= v <= hi
    ck = {n: Checksum() for n in MATH_NAMES}
    for a in vals:
        ck["sign"].add((a > 0) - (a < 0))
        ck["isqrt"].add(isqrt_model(a))
        if guarded and fits(abs(a)):
            ck["abs"].add(abs(a))
        if guarded:
            for e in range(8):
                if abs(a) <= 255 and fits(a ** e):
                    ck["pow"].add(a ** e)
        for b in vals:
            ck["min"].add(min(a, b))
            ck["max"].add(max(a, b))
            for kind, v in (("cadd", a + b), ("csub", a - b), ("cmul", a * b)):
                ck[kind].add(v if fits(v) else 65536)
            if not guarded:
                continue
            if fits(abs(a - b)):
                ck["abs_diff"].add(abs(a - b))
            if b != 0 and fits(tdiv(a, b)):
                q, r = tdiv(a, b), trem(a, b)
                cq = q + 1 if (r != 0 and ((r < 0) == (b < 0))) else q
                ck["div_ceil"].add(cq)
                ck["rem_euclid"].add(a % abs(b))
                if b > 0 and fits(cq * b):
                    ck["align_up"].add(cq * b)
            if fits(abs(a)) and fits(abs(b)):
                g = gcd_model(a, b)
                ck["gcd"].add(g)
                if a == 0 or b == 0:
                    ck["lcm"].add(0)
                elif fits(abs(a // g * b)):
                    ck["lcm"].add(abs(a // g * b))
    small = vals[::7] if len(vals) > 100 else vals[::2]
    for x in vals:
        for l in small:
            for h in small:
                ck["clamp"].add(l if x < l else (h if x > h else x))
    return ck, small


def isqrt_model(x):
    import math
    return math.isqrt(x) if x > 0 else 0


def gcd_model(a, b):
    import math
    return math.gcd(a, b)


def math_program(t, vals, small, guarded):
    lo, hi = tbounds(t)
    src = MATH_HEAD
    if guarded:
        src += MATH_GUARD_FUNCS.replace("@TMIN@", lit(lo)).replace("@TMAX@", lit(hi))
    src += table("VALS", t, vals) + table("SMALL", t, small)
    body = []
    body.append("fn main() -> i32 {")
    for n in MATH_NAMES:
        body.append("    var c_%s: u32 = 0;" % n)
    body.append("    var i: usize = 0;")
    body.append("    while i < %d {" % len(vals))
    body.append("        let a: %s = VALS[i];" % t)
    if guarded:
        body.append("        let ai = a as i64;")
        body.append("        if fits(abs64(ai)) { c_abs = mix(c_abs, math::abs(a) as u32); }")
    body.append("        c_sign = mix(c_sign, math::sign(a) as u32);")
    body.append("        c_isqrt = mix(c_isqrt, math::isqrt(a) as u32);")
    if guarded:
        body.append("        var e: u32 = 0;")
        body.append("        while e < 8 {")
        body.append("            if abs64(ai) <= 255 && fits(pow64(ai, e)) { c_pow = mix(c_pow, math::pow(a, e) as u32); }")
        body.append("            e = e + 1;")
        body.append("        }")
    body.append("        var j: usize = 0;")
    body.append("        while j < %d {" % len(vals))
    body.append("            let b: %s = VALS[j];" % t)
    body.append("            c_min = mix(c_min, math::min(a, b) as u32);")
    body.append("            c_max = mix(c_max, math::max(a, b) as u32);")
    if guarded:
        body.append("            let bi = b as i64;")
        body.append("            if fits(abs64(ai - bi)) { c_abs_diff = mix(c_abs_diff, math::abs_diff(a, b) as u32); }")
        body.append("            if bi != 0 && fits(ai / bi) {")
        body.append("                c_div_ceil = mix(c_div_ceil, math::div_ceil(a, b) as u32);")
        body.append("                c_rem_euclid = mix(c_rem_euclid, math::rem_euclid(a, b) as u32);")
        body.append("                if bi > 0 && fits(ceil_div64(ai, bi) * bi) { c_align_up = mix(c_align_up, math::align_up(a, b) as u32); }")
        body.append("            }")
        body.append("            if fits(abs64(ai)) && fits(abs64(bi)) {")
        body.append("                c_gcd = mix(c_gcd, math::gcd(a, b) as u32);")
        body.append("                if ai == 0 || bi == 0 { c_lcm = mix(c_lcm, math::lcm(a, b) as u32); }")
        body.append("                else if fits(abs64(ai / gcd64(ai, bi) * bi)) { c_lcm = mix(c_lcm, math::lcm(a, b) as u32); }")
        body.append("            }")
    for fn, c in (("checked_add", "cadd"), ("checked_sub", "csub"), ("checked_mul", "cmul")):
        body.append("            match math::%s(a, b) { Option::Some(v) => { c_%s = mix(c_%s, v as u32); } Option::None => { c_%s = mix(c_%s, 65536); } }" % (fn, c, c, c, c))
    body.append("            j = j + 1;")
    body.append("        }")
    body.append("        i = i + 1;")
    body.append("    }")
    body.append("    i = 0;")
    body.append("    while i < %d {" % len(vals))
    body.append("        var l: usize = 0;")
    body.append("        while l < %d {" % len(small))
    body.append("            var h: usize = 0;")
    body.append("            while h < %d {" % len(small))
    body.append("                c_clamp = mix(c_clamp, math::clamp(VALS[i], SMALL[l], SMALL[h]) as u32);")
    body.append("                h = h + 1;")
    body.append("            }")
    body.append("            l = l + 1;")
    body.append("        }")
    body.append("        i = i + 1;")
    body.append("    }")
    for n in MATH_NAMES:
        body.append("    print(c_%s as i32);" % n)
    body.append("    return 0;")
    body.append("}")
    return src + "\n".join(body) + "\n"


def suite_math(rng):
    jobs = []
    for t in ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"):
        signed, bits = INT_TYPES[t]
        guarded = bits <= 16
        vals = value_grid(t)
        ck, small = math_model(t, vals, guarded)
        want = "".join("%d\n" % ck[n].value() for n in MATH_NAMES)
        jobs.append(("math_" + t, {"main.nge": math_program(t, vals, small, guarded)}, want))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# a builder for the common shape: apply functions to many (a, b) pairs and print one checksum per function
# ---------------------------------------------------------------------------------------------------------------
def wrap(t, v):
    """v reduced to the type t (two's complement)."""
    signed, bits = INT_TYPES[t]
    v &= (1 << bits) - 1
    return v - (1 << bits) if signed and v >> (bits - 1) else v


class Fn:
    """name: label; expr: a Noge expression of a and b; model(a, b): the expected integer (None: skip this pair);
    guard: Noge condition under which the pair is used; wide: the result is 64 bits (both halves are mixed in)."""
    def __init__(self, name, expr, model, guard="true", wide=False):
        self.name, self.expr, self.model, self.guard, self.wide = name, expr, model, guard, wide


def pair_program(imports, ta, va, tb, vb, funcs, decls=""):
    src = "".join('import "%s" as %s;\n' % (path, alias) for alias, path in imports)
    src += "\nfn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }\n" + decls + "\n"
    src += table("VA", ta, va)
    if tb:
        src += table("VB", tb, vb)
    out = ["fn main() -> i32 {"]
    for k, f in enumerate(funcs):
        out.append("    var c%d: u32 = 0;" % k)
    out.append("    var i: usize = 0;")
    out.append("    while i < %d {" % len(va))
    out.append("        let a: %s = VA[i];" % ta)
    if tb:
        out.append("        var j: usize = 0;")
        out.append("        while j < %d {" % len(vb))
        out.append("            let b: %s = VB[j];" % tb)
    pad = "            " if tb else "        "
    for k, f in enumerate(funcs):
        body = "c%d = mix(c%d, (%s) as u32);" % (k, k, f.expr)
        if f.wide:
            body = "let r%d = %s; c%d = mix(mix(c%d, r%d as u32), (r%d >> 32) as u32);" % (k, f.expr, k, k, k, k)
        if f.guard == "true":
            out.append(pad + body)
        else:
            out.append(pad + "if %s { %s }" % (f.guard, body))
    if tb:
        out.append("            j = j + 1;")
        out.append("        }")
    out.append("        i = i + 1;")
    out.append("    }")
    for k, f in enumerate(funcs):
        out.append("    print(c%d as i32);" % k)
    out.append("    return 0;")
    out.append("}")
    return src + "\n".join(out) + "\n"


def pair_expected(va, vb, funcs):
    cks = [Checksum() for _ in funcs]
    for a in va:
        for b in (vb if vb else [None]):
            for k, f in enumerate(funcs):
                r = f.model(a, b)
                if r is None:
                    continue
                cks[k].add(r & M32)
                if f.wide:
                    cks[k].add((r >> 32) & M32)
    return "".join("%d\n" % c.value() for c in cks)


def random_words(rng, bits, n):
    """Edge values, powers of two and neighbours, sparse and dense words, and plain random ones."""
    m = (1 << bits) - 1
    vals = {0, 1, 2, 3, m, m - 1, m >> 1, (m >> 1) + 1, 0x55555555 & m, 0xAAAAAAAA & m, 0x0F0F0F0F & m}
    for k in range(bits):
        vals |= {(1 << k) & m, ((1 << k) - 1) & m, ((1 << k) + 1) & m}
    for _ in range(n):
        vals.add(rng.getrandbits(bits))
        vals.add(rng.getrandbits(bits) & rng.getrandbits(bits) & rng.getrandbits(bits))
        vals.add(rng.getrandbits(bits) | rng.getrandbits(bits) | rng.getrandbits(bits))
    return sorted(vals)


# ---------------------------------------------------------------------------------------------------------------
# bits
# ---------------------------------------------------------------------------------------------------------------
def rotl_model(x, n, bits):
    k = n % bits
    m = (1 << bits) - 1
    return ((x << k) | (x >> (bits - k))) & m if k else x


def swap_model(x, bits):
    return int.from_bytes(x.to_bytes(bits // 8, "little"), "big")


def rev_model(x, bits):
    return int(format(x, "0%db" % bits)[::-1], 2)


def suite_bits(rng):
    jobs = []
    for bits, sfx, t in ((32, "", "u32"), (64, "64", "u64")):
        vals = random_words(rng, bits, 120)
        ns = [0, 1, 2, 5, 7, 8, 15, 16, 17, 31, 32, 33, 40, 63, 64, 65, 100]
        m = (1 << bits) - 1
        fs = [
            Fn("popcount", "bits::popcount%s(a)" % sfx, lambda a, b: bin(a).count("1")),
            Fn("clz", "bits::clz%s(a)" % sfx, lambda a, b: bits - a.bit_length()),
            Fn("ctz", "bits::ctz%s(a)" % sfx, lambda a, b: bits if a == 0 else (a & -a).bit_length() - 1),
            Fn("rotl", "bits::rotl%s(a, b)" % sfx, lambda a, b: rotl_model(a, b, bits), wide=bits == 64),
            Fn("rotr", "bits::rotr%s(a, b)" % sfx, lambda a, b: rotl_model(a, (bits - b % bits) % bits, bits), wide=bits == 64),
            Fn("swap_bytes", "bits::swap_bytes%s(a)" % sfx, lambda a, b: swap_model(a, bits), wide=bits == 64),
            Fn("reverse_bits", "bits::reverse_bits%s(a)" % sfx, lambda a, b: rev_model(a, bits), wide=bits == 64),
        ]
        if bits == 32:
            fs += [
                Fn("log2", "bits::log2(a)", lambda a, b: a.bit_length() - 1 if a else None, guard="a != 0"),
                Fn("next_pow2", "bits::next_pow2(a)", lambda a, b: 1 if a <= 1 else (1 << (a - 1).bit_length()) if a <= 1 << 31 else None, guard="a <= 0x8000_0000"),
            ]
        # the shift amounts are u32 for rotl/rotr, so the b table is u32 for both widths
        prog = pair_program([("bits", "std/bits.nge")], t, vals, "u32", ns, fs)
        # one-argument functions would be repeated len(ns) times: fine, the model repeats them as well
        jobs.append(("bits_%d" % bits, {"main.nge": prog}, pair_expected(vals, ns, fs)))
    # 16 and 8 bits
    v16 = random_words(rng, 16, 60)
    fs = [Fn("swap16", "bits::swap_bytes16(a)", lambda a, b: swap_model(a, 16)),
          Fn("rev8", "bits::reverse_bits8(a as u8)", lambda a, b: rev_model(a & 0xFF, 8))]
    jobs.append(("bits_16", {"main.nge": pair_program([("bits", "std/bits.nge")], "u16", v16, None, None, fs)}, pair_expected(v16, None, fs)))
    # fields
    vals = random_words(rng, 32, 40)
    los = [0, 1, 3, 4, 7, 8, 12, 16, 20, 24, 28, 31]
    widths = [0, 1, 2, 3, 4, 5, 8, 12, 16, 24, 31, 32]
    cases = [(lo, w) for lo in los for w in widths]
    lov = [c[0] for c in cases]
    wv = [c[1] for c in cases]
    decls = table("LO", "u32", lov) + table("W", "u32", wv)
    def fld(a, k):
        lo, w = cases[k]
        mk = (1 << w) - 1 if w < 32 else M32
        return (a >> lo) & mk
    def setf(a, k):
        lo, w = cases[k]
        mk = (1 << w) - 1 if w < 32 else M32
        return ((a & ~((mk << lo) & M32)) | (((0xA5C3_9E71 & mk) << lo) & M32)) & M32
    ks = list(range(len(cases)))
    fs = [Fn("field", "bits::field(a, LO[b], W[b])", fld),
          Fn("set_field", "bits::set_field(a, LO[b], W[b], 0xA5C3_9E71)", setf)]
    jobs.append(("bits_fields", {"main.nge": pair_program([("bits", "std/bits.nge")], "u32", vals, "usize", ks, fs, decls)}, pair_expected(vals, ks, fs)))
    # single bits in every integer type
    for t in ("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"):
        signed, nb = INT_TYPES[t]
        lo, hi = tbounds(t)
        xs = list(range(lo, hi + 1)) if nb == 8 else sorted({wrap(t, v) for v in random_words(rng, nb, 40)} | ({-1, -2, lo, hi} if signed else {hi}))
        ns = list(range(nb))
        one = 1
        fs = [Fn("get", "bits::get(a, b)", lambda a, b: ((a & ((1 << nb) - 1)) >> b) & 1),
              Fn("set", "bits::set(a, b)", lambda a, b: wrap(t, a | (1 << b)), wide=nb == 64),
              Fn("clear", "bits::clear(a, b)", lambda a, b: wrap(t, a & ~(1 << b)), wide=nb == 64),
              Fn("toggle", "bits::toggle(a, b)", lambda a, b: wrap(t, a ^ (1 << b)), wide=nb == 64),
              Fn("put_on", "bits::put(a, b, true)", lambda a, b: wrap(t, a | (1 << b)), wide=nb == 64),
              Fn("put_off", "bits::put(a, b, false)", lambda a, b: wrap(t, a & ~(1 << b)), wide=nb == 64),
              Fn("is_pow2", "bits::is_pow2(a)", lambda a, b: int(a > 0 and a & (a - 1) == 0))]
        jobs.append(("bits_single_" + t, {"main.nge": pair_program([("bits", "std/bits.nge")], t, xs, t, ns, fs)}, pair_expected(xs, ns, fs)))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# array
# ---------------------------------------------------------------------------------------------------------------
ARRAY_NAMES = ["sort", "heap_sort", "sorted_flags", "min_index", "max_index", "sum", "reverse", "rotate", "index_of",
               "contains", "count", "binary_search", "insert", "remove", "copy", "equal", "fill", "swap_at"]


def array_program(t, N, rows, needles):
    R = len(rows)
    ks = [0, 1, 2, 3, N, N + 1, 2 * N + 3]
    src = """import "std/array.nge" as array;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn enc(o: Option<usize>) -> u32 {
    return match o { Option::Some(i) => i as u32 + 1, Option::None => 0 };
}
fn encr(r: Result<usize, usize>) -> u32 {
    return match r { Result::Ok(i) => i as u32, Result::Err(i) => 1000 + i as u32 };
}
fn mix_all(acc: u32, a: [T; N]) -> u32 {
    var c = acc;
    var i: usize = 0;
    while i < len(a) { c = mix(c, a[i] as u32); i = i + 1; }
    return c;
}
""".replace("[T; N]", "[%s; %d]" % (t, N))
    src += "static ROWS: [[%s; %d]; %d] = [%s];\n" % (t, N, R, ", ".join("[" + ", ".join(map(str, r)) + "]" for r in rows))
    src += table("NEEDLES", t, needles) + table("KS", "usize", ks)
    body = ["fn main() -> i32 {"]
    for n in ARRAY_NAMES:
        body.append("    var c_%s: u32 = 0;" % n)
    body.append("    var r: usize = 0;")
    body.append("    while r < %d {" % R)
    body.append("        var n: usize = 0;")
    body.append("        while n <= %d {" % N)
    body.append("            let orig: [%s; %d] = ROWS[r];" % (t, N))
    body.append("            var s = orig; array::sort(&s, n); c_sort = mix_all(c_sort, s);")
    body.append("            var h = orig; array::heap_sort(&h, n); c_heap_sort = mix_all(c_heap_sort, h);")
    body.append("            c_sorted_flags = mix(mix(c_sorted_flags, array::is_sorted(orig, n) as u32), array::is_sorted(s, n) as u32);")
    body.append("            c_min_index = mix(c_min_index, enc(array::min_index(orig, n)));")
    body.append("            c_max_index = mix(c_max_index, enc(array::max_index(orig, n)));")
    body.append("            c_sum = mix(c_sum, array::sum(orig, n) as u32);")
    body.append("            var rv = orig; array::reverse(&rv, n); c_reverse = mix_all(c_reverse, rv);")
    body.append("            var q: usize = 0;")
    body.append("            while q < %d {" % len(ks))
    body.append("                var rt = orig; array::rotate_left(&rt, n, KS[q]); c_rotate = mix_all(c_rotate, rt);")
    body.append("                q = q + 1;")
    body.append("            }")
    body.append("            var d: [%s; %d] = [9; %d];" % (t, N, N))
    body.append("            array::copy(&d, orig, n);")
    body.append("            c_copy = mix_all(c_copy, d);")
    body.append("            c_equal = mix(mix(c_equal, array::equal(orig, d, n) as u32), array::equal(orig, d, %d) as u32);" % N)
    body.append("            var f = orig; array::fill(&f, 3); c_fill = mix_all(c_fill, f);")
    body.append("            var x: usize = 0;")
    body.append("            while x < %d {" % len(needles))
    body.append("                let nd = NEEDLES[x];")
    body.append("                c_index_of = mix(c_index_of, enc(array::index_of(orig, n, nd)));")
    body.append("                c_contains = mix(c_contains, array::contains(orig, n, nd) as u32);")
    body.append("                c_count = mix(c_count, array::count(orig, n, nd) as u32);")
    body.append("                c_binary_search = mix(c_binary_search, encr(array::binary_search(s, n, nd)));")
    body.append("                var ip = orig; let okk = array::insert(&ip, n, x, nd);")
    body.append("                c_insert = mix_all(mix(c_insert, okk as u32), ip);")
    body.append("                x = x + 1;")
    body.append("            }")
    body.append("            var pos: usize = 0;")
    body.append("            while pos <= %d {" % (N + 1))
    body.append("                var rm = orig;")
    body.append("                match array::remove(&rm, n, pos) { Option::Some(v) => { c_remove = mix(mix(c_remove, 1), v as u32); } Option::None => { c_remove = mix(c_remove, 0); } }")
    body.append("                c_remove = mix_all(c_remove, rm);")
    body.append("                var sw = orig;")
    body.append("                if pos < %d { array::swap_at(&sw, pos, %d - 1 - pos); }" % (N, N))
    body.append("                c_swap_at = mix_all(c_swap_at, sw);")
    body.append("                pos = pos + 1;")
    body.append("            }")
    body.append("            n = n + 1;")
    body.append("        }")
    body.append("        r = r + 1;")
    body.append("    }")
    for n in ARRAY_NAMES:
        body.append("    print(c_%s as i32);" % n)
    body.append("    return 0;")
    body.append("}")
    return src + "\n".join(body) + "\n"


def array_expected(t, N, rows, needles):
    import bisect
    ks = [0, 1, 2, 3, N, N + 1, 2 * N + 3]
    ck = {n: Checksum() for n in ARRAY_NAMES}
    def allv(c, a):
        for v in a:
            c.add(v)
    for row in rows:
        for n in range(N + 1):
            orig = list(row)
            srt = sorted(orig[:n]) + orig[n:]
            allv(ck["sort"], srt)
            allv(ck["heap_sort"], srt)
            ck["sorted_flags"].add(int(all(orig[i] >= orig[i - 1] for i in range(1, n))))
            ck["sorted_flags"].add(1)
            ck["min_index"].add(0 if n == 0 else orig[:n].index(min(orig[:n])) + 1)
            ck["max_index"].add(0 if n == 0 else orig[:n].index(max(orig[:n])) + 1)
            ck["sum"].add(sum(orig[:n]))
            allv(ck["reverse"], orig[:n][::-1] + orig[n:])
            for k in ks:
                if n < 2:
                    allv(ck["rotate"], orig)
                else:
                    m = k % n
                    allv(ck["rotate"], orig[m:n] + orig[:m] + orig[n:])
            d = [9] * N
            d[:n] = orig[:n]
            allv(ck["copy"], d)
            ck["equal"].add(int(orig[:n] == d[:n]))
            ck["equal"].add(int(orig == d))
            allv(ck["fill"], [3] * N)
            for x, nd in enumerate(needles):
                ck["index_of"].add(orig[:n].index(nd) + 1 if nd in orig[:n] else 0)
                ck["contains"].add(int(nd in orig[:n]))
                ck["count"].add(orig[:n].count(nd))
                sp = sorted(orig[:n])
                i = bisect.bisect_left(sp, nd)
                ck["binary_search"].add(i if i < n and sp[i] == nd else 1000 + i)
                ok = n < N and x <= n
                ip = orig[:x] + [nd] + orig[x:n] + orig[n + 1:] if ok else list(orig)
                ck["insert"].add(int(ok))
                allv(ck["insert"], ip)
            for pos in range(N + 2):
                rm = list(orig)
                if pos < n:
                    ck["remove"].add(1)
                    ck["remove"].add(orig[pos])
                    rm[pos:n - 1] = orig[pos + 1:n]
                else:
                    ck["remove"].add(0)
                allv(ck["remove"], rm)
                sw = list(orig)
                if pos < N:
                    sw[pos], sw[N - 1 - pos] = sw[N - 1 - pos], sw[pos]
                allv(ck["swap_at"], sw)
    return "".join("%d\n" % ck[n].value() for n in ARRAY_NAMES)


def suite_array(rng):
    jobs = []
    for t, vlo, vhi, extra in (("i32", -5, 5, [1000, -1000, 1000000, -1000000]), ("u8", 0, 6, []), ("i8", -4, 4, [])):
        for N in (1, 2, 3, 5, 8, 17):
            rows = []
            for _ in range(45):
                rows.append([rng.randint(vlo, vhi) for _ in range(N)])
            rows.append([i % 5 for i in range(N)])
            rows.append([i % 5 for i in range(N)][::-1])
            rows.append(sorted(rng.randint(vlo, vhi) for _ in range(N)))
            rows.append([2] * N)
            if N > 2:
                rows.append([vlo if i % 2 else vhi for i in range(N)])
            if extra:
                rows.append([rng.choice(extra + [0]) for _ in range(N)])
            needles = sorted({vlo, vlo + 1, 0, 1, vhi, vhi - 1, 3}) if vlo <= 0 else [0, 1, 2, 3, 6, 5]
            needles = [v for v in needles if vlo <= v <= vhi]
            jobs.append(("array_%s_%d" % (t, N), {"main.nge": array_program(t, N, rows, needles)}, array_expected(t, N, rows, needles)))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# containers: random sequences of operations against Python lists and deques
# ---------------------------------------------------------------------------------------------------------------
OPS_HEAD = """%(imports)s
fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn mix_opt(acc: u32, o: Option<i32>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
fn mix_idx(acc: u32, o: Option<usize>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
fn mix_res(acc: u32, r: Result<usize, usize>) -> u32 {
    return match r { Result::Ok(v) => mix(mix(acc, 1), v as u32), Result::Err(v) => mix(mix(acc, 2), v as u32) };
}
static EXT: [i32; 8] = [4, -3, 9, 9, 0, 2, -7, 5];
"""
EXT = [4, -3, 9, 9, 0, 2, -7, 5]


def ops_tables(ops):
    return table("OPC", "u8", [o[0] for o in ops]) + table("OPA", "i32", [o[1] for o in ops]) + table("OPB", "i32", [o[2] for o in ops])


class Mix:
    """The model of the program's running checksum."""
    def __init__(self):
        self.c = Checksum()

    def __call__(self, v):
        self.c.add(v)

    def opt(self, v):
        if v is None:
            self(0)
        else:
            self(1)
            self(v)

    def value(self):
        return self.c.value()


# op: (code, kind of a, kind of b)  kinds: v value, i index, n count 0..8
VEC_OPS = [("push", "v", ""), ("pop", "", ""), ("get", "i", ""), ("set", "i", "v"), ("insert", "i", "v"), ("remove", "i", ""),
           ("swap_remove", "i", ""), ("truncate", "i", ""), ("clear", "", ""), ("sort", "", ""), ("reverse", "", ""),
           ("contains", "v", ""), ("index_of", "v", ""), ("first", "", ""), ("last", "", ""), ("heap_sort", "", ""),
           ("sum", "", ""), ("binary_search", "v", ""), ("count", "v", ""), ("extend", "n", ""), ("append", "n", ""),
           ("min_max", "", ""), ("flags", "", ""), ("equal", "i", "v")]
VEC_WEIGHTS = [10, 6, 4, 3, 5, 5, 3, 1, 0.3, 1, 1, 2, 2, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 2]


def random_ops(rng, table, weights, N, count):
    ops = []
    codes = list(range(len(table)))
    for _ in range(count):
        c = rng.choices(codes, weights)[0]
        def arg(kind):
            if kind == "v": return rng.randint(-3, 6)
            if kind == "i": return rng.randint(0, N + 2)
            if kind == "n": return rng.randint(0, 8)
            return 0
        ops.append((c, arg(table[c][1]), arg(table[c][2])))
    return ops


def vec_program(N, ops):
    src = OPS_HEAD % {"imports": 'import "std/vec.nge" as vec;'} + ops_tables(ops)
    src += """
fn mix_vec(acc: u32, v: vec::Vec<i32, %d>) -> u32 {
    var c = mix(acc, v.len as u32);
    var i: usize = 0;
    while i < v.len { c = mix(c, v.items[i] as u32); i = i + 1; }
    return c;
}

fn main() -> i32 {
    var v: vec::Vec<i32, %d> = vec::new(-1);
    var c: u32 = 0;
    var k: usize = 0;
    while k < %d {
        let op = OPC[k];
        let a = OPA[k];
        let b = OPB[k];
        let ua = a as usize;
""" % (N, N, len(ops))
    chain = [
        "c = mix(c, vec::push(&v, a) as u32);",
        "c = mix_opt(c, vec::pop(&v));",
        "c = mix_opt(c, vec::get(v, ua));",
        "c = mix(c, vec::set(&v, ua, b) as u32);",
        "c = mix(c, vec::insert(&v, ua, b) as u32);",
        "c = mix_opt(c, vec::remove(&v, ua));",
        "c = mix_opt(c, vec::swap_remove(&v, ua));",
        "vec::truncate(&v, ua);",
        "vec::clear(&v);",
        "vec::sort(&v);",
        "vec::reverse(&v);",
        "c = mix(c, vec::contains(v, a) as u32);",
        "c = mix_idx(c, vec::index_of(v, a));",
        "c = mix_opt(c, vec::first(v));",
        "c = mix_opt(c, vec::last(v));",
        "vec::heap_sort(&v);",
        "c = mix(c, vec::sum(v) as u32);",
        "c = mix_res(c, vec::binary_search(v, a));",
        "c = mix(c, vec::count(v, a) as u32);",
        "c = mix(c, vec::extend(&v, EXT, ua) as u32);",
        "var w: vec::Vec<i32, 8> = vec::new(0); vec::extend(&w, EXT, ua); c = mix(c, vec::append(&v, w) as u32);",
        "c = mix_idx(mix_idx(c, vec::min_index(v)), vec::max_index(v));",
        "c = mix(mix(mix(mix(c, vec::is_sorted(v) as u32), vec::is_full(v) as u32), vec::is_empty(v) as u32), (vec::size(v) + vec::capacity(v)) as u32);",
        "var u: vec::Vec<i32, %d> = vec::new(0); vec::append(&u, v); if ua < u.len { u.items[ua] = b; } c = mix(mix(c, vec::equal(v, u) as u32), vec::equal(v, v) as u32); var t = v; vec::truncate(&t, ua); c = mix(mix(c, vec::equal(v, t) as u32), vec::equal(t, v) as u32);" % (N + 3),
    ]
    for i, text in enumerate(chain):
        src += "        %sif op == %d { %s }\n" % ("" if i == 0 else "else ", i, text)
    src += """        c = mix_vec(c, v);
        k = k + 1;
    }
    print(c as i32);
    return 0;
}
"""
    return src


def vec_expected(N, ops):
    import bisect
    v = []
    m = Mix()
    for op, a, b in ops:
        ua = a
        if op == 0:
            ok = len(v) < N
            m(int(ok))
            if ok: v.append(a)
        elif op == 1:
            m.opt(v.pop() if v else None)
        elif op == 2:
            m.opt(v[ua] if ua < len(v) else None)
        elif op == 3:
            ok = ua < len(v)
            m(int(ok))
            if ok: v[ua] = b
        elif op == 4:
            ok = len(v) < N and ua <= len(v)
            m(int(ok))
            if ok: v.insert(ua, b)
        elif op == 5:
            m.opt(v.pop(ua) if ua < len(v) else None)
        elif op == 6:
            if ua < len(v):
                x = v[ua]
                v[ua] = v[-1]
                v.pop()
                m.opt(x)
            else:
                m.opt(None)
        elif op == 7:
            if ua < len(v): del v[ua:]
        elif op == 8:
            v.clear()
        elif op in (9, 15):
            v.sort()
        elif op == 10:
            v.reverse()
        elif op == 11:
            m(int(a in v))
        elif op == 12:
            m.opt(v.index(a) if a in v else None)
        elif op == 13:
            m.opt(v[0] if v else None)
        elif op == 14:
            m.opt(v[-1] if v else None)
        elif op == 16:
            m(sum(v))
        elif op == 17:
            i = bisect.bisect_left(v, a)  # the same probing sequence as the library, even for an unsorted vector
            if i < len(v) and v[i] == a:
                m(1); m(i)
            else:
                m(2); m(i)
        elif op == 18:
            m(v.count(a))
        elif op == 19:
            ok = ua <= N - len(v)
            m(int(ok))
            if ok: v.extend(EXT[:ua])
        elif op == 20:
            w = EXT[:ua]
            ok = len(w) <= N - len(v)
            m(int(ok))
            if ok: v.extend(w)
        elif op == 21:
            m.opt(v.index(min(v)) if v else None)
            m.opt(v.index(max(v)) if v else None)
        elif op == 22:
            m(int(all(v[i] <= v[i + 1] for i in range(len(v) - 1))))
            m(int(len(v) == N)); m(int(len(v) == 0)); m(len(v) + N)
        elif op == 23:
            u = list(v)
            if ua < len(u): u[ua] = b
            m(int(v == u)); m(1)
            t = v[:ua]
            m(int(v == t)); m(int(t == v))
        m(len(v))
        for x in v: m(x)
    return "%d\n" % m.value()


def suite_vec(rng):
    jobs = []
    for N in (1, 3, 8, 16):
        ops = random_ops(rng, VEC_OPS, VEC_WEIGHTS, N, 2500)
        jobs.append(("vec_%d" % N, {"main.nge": vec_program(N, ops)}, vec_expected(N, ops)))
    return jobs


RING_OPS = [("push", "v", ""), ("pop", "", ""), ("push_front", "v", ""), ("pop_back", "", ""), ("push_overwrite", "v", ""),
            ("peek", "", ""), ("peek_back", "", ""), ("get", "i", ""), ("clear", "", ""), ("flags", "", "")]
RING_WEIGHTS = [9, 8, 5, 5, 4, 2, 2, 3, 0.3, 1]


def ring_program(N, ops):
    src = OPS_HEAD % {"imports": 'import "std/ring.nge" as ring;'} + ops_tables(ops)
    src += """
fn main() -> i32 {
    var r: ring::Ring<i32, %d> = ring::new(-1);
    var c: u32 = 0;
    var k: usize = 0;
    while k < %d {
        let op = OPC[k];
        let a = OPA[k];
        let ua = a as usize;
""" % (N, len(ops))
    chain = [
        "c = mix(c, ring::push(&r, a) as u32);",
        "c = mix_opt(c, ring::pop(&r));",
        "c = mix(c, ring::push_front(&r, a) as u32);",
        "c = mix_opt(c, ring::pop_back(&r));",
        "c = mix_opt(c, ring::push_overwrite(&r, a));",
        "c = mix_opt(c, ring::peek(r));",
        "c = mix_opt(c, ring::peek_back(r));",
        "c = mix_opt(c, ring::get(r, ua));",
        "ring::clear(&r);",
        "c = mix(mix(mix(c, ring::is_full(r) as u32), ring::is_empty(r) as u32), ring::capacity(r) as u32);",
    ]
    for i, text in enumerate(chain):
        src += "        %sif op == %d { %s }\n" % ("" if i == 0 else "else ", i, text)
    src += """        c = mix(c, ring::size(r) as u32);
        var i: usize = 0;
        while i < ring::size(r) { c = mix_opt(c, ring::get(r, i)); i = i + 1; }
        k = k + 1;
    }
    var d = r;
    var more = true;
    while more {
        match ring::pop(&d) {
            Option::Some(x) => { c = mix(c, x as u32); }
            Option::None => { more = false; }
        }
    }
    print(c as i32);
    return 0;
}
"""
    return src


def ring_expected(N, ops):
    from collections import deque
    q = deque()
    m = Mix()
    for op, a, b in ops:
        ua = a
        if op == 0:
            ok = len(q) < N
            m(int(ok))
            if ok: q.append(a)
        elif op == 1:
            m.opt(q.popleft() if q else None)
        elif op == 2:
            ok = len(q) < N
            m(int(ok))
            if ok: q.appendleft(a)
        elif op == 3:
            m.opt(q.pop() if q else None)
        elif op == 4:
            dropped = q.popleft() if len(q) == N else None
            q.append(a)
            m.opt(dropped)
        elif op == 5:
            m.opt(q[0] if q else None)
        elif op == 6:
            m.opt(q[-1] if q else None)
        elif op == 7:
            m.opt(q[ua] if ua < len(q) else None)
        elif op == 8:
            q.clear()
        elif op == 9:
            m(int(len(q) == N)); m(int(len(q) == 0)); m(N)
        m(len(q))
        for x in q:
            m(1); m(x)
    for x in list(q):
        m(x)
    return "%d\n" % m.value()


def suite_ring(rng):
    jobs = []
    for N in (1, 2, 3, 7, 16):
        ops = random_ops(rng, RING_OPS, RING_WEIGHTS, N, 3000)
        jobs.append(("ring_%d" % N, {"main.nge": ring_program(N, ops)}, ring_expected(N, ops)))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# rand: PCG32 as in the reference implementation, and xorshift32
# ---------------------------------------------------------------------------------------------------------------
class PCG32:
    MULT = 6364136223846793005

    def __init__(self, initstate, initseq):
        self.state = 0
        self.inc = ((initseq << 1) | 1) & M64
        self.next()
        self.state = (self.state + initstate) & M64
        self.next()

    def next(self):
        old = self.state
        self.state = (old * self.MULT + self.inc) & M64
        xorshifted = (((old >> 18) ^ old) >> 27) & M32
        rot = old >> 59
        return ((xorshifted >> rot) | (xorshifted << ((-rot) & 31))) & M32

    def below(self, n):
        threshold = ((1 << 32) - n) % n
        while True:
            x = self.next()
            if x >= threshold:
                return x % n


# the output of the PCG reference demo for pcg32_srandom(42, 54); the model must reproduce it before it is trusted
_ref = PCG32(42, 54)
assert [_ref.next() for _ in range(6)] == [0xa15c02b7, 0x7b47f409, 0xba1d3330, 0x83d2f293, 0xbfa4784b, 0xcbed606e], "PCG32 model is wrong"


def suite_rand(rng):
    seeds = [(0, 54), (1, 54), (42, 54), (12345, 1), (M64, 7), (0x1234_5678_9ABC_DEF0, M64), (999, 0)]
    bns = [1, 2, 3, 6, 10, 100, 1000, 65536, 3_000_000_000, M32]
    rngs = [(0, 6), (-5, 5), (-2147483648, 2147483647), (7, 7), (9, 3), (-100, -90), (2147483600, 2147483647)]
    prog = """import "std/rand.nge" as rand;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
""" + table("INITSTATE", "u64", [x[0] for x in seeds]) + table("INITSEQ", "u64", [x[1] for x in seeds]) \
        + table("BNS", "u32", bns) + table("LOS", "i32", [x[0] for x in rngs]) + table("HIS", "i32", [x[1] for x in rngs]) + """
fn main() -> i32 {
    var total: u32 = 0;
    var s: usize = 0;
    while s < %d {
        var r = rand::seed_stream(INITSTATE[s], INITSEQ[s]);
        var c_next: u32 = 0; var c_below: u32 = 0; var c_range: u32 = 0; var c_coin: u32 = 0; var c_u64: u32 = 0;
        var c_shuffle: u32 = 0; var c_xs: u32 = 0; var c_seed: u32 = 0;
        var i: u32 = 0;
        while i < 50 { c_next = mix(c_next, rand::next(&r)); i = i + 1; }
        var k: usize = 0;
        while k < %d {
            i = 0;
            while i < 20 { c_below = mix(c_below, rand::below(&r, BNS[k])); i = i + 1; }
            k = k + 1;
        }
        k = 0;
        while k < %d {
            i = 0;
            while i < 20 { c_range = mix(c_range, rand::range(&r, LOS[k], HIS[k]) as u32); i = i + 1; }
            k = k + 1;
        }
        i = 0;
        while i < 30 {
            c_coin = mix(mix(c_coin, rand::coin(&r) as u32), rand::chance(&r, 1, 3) as u32);
            i = i + 1;
        }
        i = 0;
        while i < 10 {
            let w = rand::next_u64(&r);
            c_u64 = mix(mix(c_u64, w as u32), (w >> 32) as u32);
            i = i + 1;
        }
        var round: u32 = 0;
        while round < 5 {
            var deck: [i32; 10] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
            let count: usize = if_len(round);
            rand::shuffle(&r, &deck, count);
            var j: usize = 0;
            while j < 10 { c_shuffle = mix(c_shuffle, deck[j] as u32); j = j + 1; }
            round = round + 1;
        }
        var xs: u32 = (INITSTATE[s] as u32) | 1;
        i = 0;
        while i < 40 { c_xs = mix(c_xs, rand::xorshift32(&xs)); i = i + 1; }
        var q = rand::seed(INITSTATE[s]);
        i = 0;
        while i < 5 { c_seed = mix(c_seed, rand::next(&q)); i = i + 1; }
        print(c_next as i32); print(c_below as i32); print(c_range as i32); print(c_coin as i32);
        print(c_u64 as i32); print(c_shuffle as i32); print(c_xs as i32); print(c_seed as i32);
        total = mix(total, rand::next(&r));
        s = s + 1;
    }
    print(total as i32);
    // a draw that lands exactly on the rejection threshold is accepted: the second output of the reference stream,
    // 2068313097, is the threshold for n = 2^32 - 2068313097
    var e = rand::seed_stream(42, 54);
    rand::next(&e);
    print(rand::below(&e, 2226654199) as i32);
    print(rand::next(&e) as i32);
    return 0;
}

fn if_len(round: u32) -> usize {
    if round == 0 { return 10; }
    if round == 1 { return 1; }
    if round == 2 { return 0; }
    if round == 3 { return 7; }
    return 10;
}
""" % (len(seeds), len(bns), len(rngs))
    out = ""
    total = Checksum()
    for st, sq in seeds:
        r = PCG32(st, sq)
        c_next = Checksum()
        for _ in range(50): c_next.add(r.next())
        c_below = Checksum()
        for n in bns:
            for _ in range(20): c_below.add(r.below(n))
        c_range = Checksum()
        for lo, hi in rngs:
            for _ in range(20):
                if hi <= lo:
                    c_range.add(lo)
                else:
                    c_range.add(lo + r.below(hi - lo))
        c_coin = Checksum()
        for _ in range(30):
            c_coin.add(r.next() >> 31)
            c_coin.add(int(r.below(3) < 1))
        c_u64 = Checksum()
        for _ in range(10):
            hi_, lo_ = r.next(), r.next()
            c_u64.add(lo_)
            c_u64.add(hi_)
        c_shuffle = Checksum()
        for count in (10, 1, 0, 7, 10):
            deck = list(range(10))
            i = count
            while i > 1:
                i -= 1
                j = r.below(i + 1)
                deck[i], deck[j] = deck[j], deck[i]
            for v in deck: c_shuffle.add(v)
        c_xs = Checksum()
        x = (st & M32) | 1
        for _ in range(40):
            x ^= (x << 13) & M32
            x ^= x >> 17
            x ^= (x << 5) & M32
            c_xs.add(x)
        q = PCG32(st, 54)
        c_seed = Checksum()
        for _ in range(5): c_seed.add(q.next())
        for c in (c_next, c_below, c_range, c_coin, c_u64, c_shuffle, c_xs, c_seed):
            out += "%d\n" % c.value()
        total.add(r.next())
    out += "%d\n" % total.value()
    e = PCG32(42, 54)
    e.next()
    assert (1 << 32) - 2068313097 == 2226654199 and ((1 << 32) - 2226654199) % 2226654199 == 2068313097
    out += "%d\n" % s32(e.below(2226654199))
    out += "%d\n" % s32(e.next())
    return [("rand", {"main.nge": prog}, out)]


# ---------------------------------------------------------------------------------------------------------------
# hash
# ---------------------------------------------------------------------------------------------------------------
def crc16_ccitt_model(data):
    return binascii.crc_hqx(bytes(data), 0xFFFF)


def crc16_modbus_model(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def crc8_model(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def fnv1a_model(data, bits):
    h = 0x811C9DC5 if bits == 32 else 0xCBF29CE484222325
    prime = 16777619 if bits == 32 else 0x100000001B3
    m = (1 << bits) - 1
    for b in data:
        h = ((h ^ b) * prime) & m
    return h


def xor8_model(data):
    x = 0
    for b in data:
        x ^= b
    return x


assert zlib.crc32(b"123456789") == 0xCBF43926 and crc16_ccitt_model(b"123456789") == 0x29B1
assert crc16_modbus_model(b"123456789") == 0x4B37 and crc8_model(b"123456789") == 0xF4 and zlib.adler32(b"Wikipedia") == 0x11E60398
assert fnv1a_model(b"a", 32) == 0xE40C292C and fnv1a_model(b"a", 64) == 0xAF63DC4C8601EC8C


HASH_FUNCS = [
    ("crc32", "hash::crc32(a, n)", lambda d: zlib.crc32(bytes(d)), False),
    ("crc32_fast", "hash::crc32_fast(a, n)", lambda d: zlib.crc32(bytes(d)), False),
    ("crc16_ccitt", "hash::crc16_ccitt(a, n)", crc16_ccitt_model, False),
    ("crc16_modbus", "hash::crc16_modbus(a, n)", crc16_modbus_model, False),
    ("crc8", "hash::crc8(a, n)", crc8_model, False),
    ("adler32", "hash::adler32(a, n)", lambda d: zlib.adler32(bytes(d)), False),
    ("fnv1a32", "hash::fnv1a32(a, n)", lambda d: fnv1a_model(d, 32), False),
    ("fnv1a64", "hash::fnv1a64(a, n)", lambda d: fnv1a_model(d, 64), True),
    ("xor8", "hash::xor8(a, n)", xor8_model, False),
]


def suite_hash(rng):
    jobs = []
    for N in (1, 5, 64, 300):
        nsel = sorted({0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64, 100, 255, 256, 299, 300} & set(range(N + 1)) | {N})
        rows = [[rng.randrange(256) for _ in range(N)] for _ in range(12)]
        rows.append([0] * N)
        rows.append([255] * N)
        rows.append([i & 255 for i in range(N)])
        fs = []
        for name, expr, model, wide in HASH_FUNCS:
            fs.append((name, expr, model, wide))
        src = 'import "std/hash.nge" as hash;\n\nfn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }\n'
        src += "static ROWS: [[u8; %d]; %d] = [%s];\n" % (N, len(rows), ", ".join("[" + ", ".join(map(str, r)) + "]" for r in rows))
        src += table("NS", "usize", nsel)
        src += "\nfn main() -> i32 {\n"
        for k in range(len(fs)):
            src += "    var c%d: u32 = 0;\n" % k
        src += "    var r: usize = 0;\n    while r < %d {\n        let a: [u8; %d] = ROWS[r];\n        var q: usize = 0;\n        while q < %d {\n            let n = NS[q];\n" % (len(rows), N, len(nsel))
        for k, (name, expr, model, wide) in enumerate(fs):
            if wide:
                src += "            let w%d = %s; c%d = mix(mix(c%d, w%d as u32), (w%d >> 32) as u32);\n" % (k, expr, k, k, k, k)
            else:
                src += "            c%d = mix(c%d, %s as u32);\n" % (k, k, expr)
        src += "            q = q + 1;\n        }\n        r = r + 1;\n    }\n"
        # streaming: one byte at a time must give the same as the whole buffer
        src += """    var stream: u32 = 0;
    r = 0;
    while r < %d {
        let a: [u8; %d] = ROWS[r];
        var c32 = hash::CRC32_INIT;
        var c32f = hash::CRC32_INIT;
        var f32 = hash::FNV32_INIT;
        var i: usize = 0;
        while i < %d {
            c32 = hash::crc32_update(c32, a[i]);
            c32f = hash::crc32_fast_update(c32f, a[i]);
            f32 = hash::fnv1a32_update(f32, a[i]);
            i = i + 1;
        }
        stream = mix(mix(mix(stream, hash::crc32_finish(c32)), hash::crc32_finish(c32f)), f32);
        r = r + 1;
    }
""" % (len(rows), N, N)
        for k in range(len(fs)):
            src += "    print(c%d as i32);\n" % k
        src += "    print(stream as i32);\n    return 0;\n}\n"
        out = ""
        for name, expr, model, wide in fs:
            c = Checksum()
            for row in rows:
                for n in nsel:
                    v = model(row[:n])
                    c.add(v & M32)
                    if wide: c.add((v >> 32) & M32)
            out += "%d\n" % c.value()
        st = Checksum()
        for row in rows:
            st.add(zlib.crc32(bytes(row)))
            st.add(zlib.crc32(bytes(row)))
            st.add(fnv1a_model(row, 32))
        out += "%d\n" % st.value()
        jobs.append(("hash_%d" % N, {"main.nge": src}, out))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# ascii: all 256 byte values
# ---------------------------------------------------------------------------------------------------------------
import string as _string
PUNCT = set(ord(c) for c in _string.punctuation)


def suite_ascii(rng):
    def opt(v): return 0 if v is None else v + 1
    fs = [
        Fn("is_digit", "ascii::is_digit(a)", lambda a, b: int(48 <= a <= 57)),
        Fn("is_lower", "ascii::is_lower(a)", lambda a, b: int(97 <= a <= 122)),
        Fn("is_upper", "ascii::is_upper(a)", lambda a, b: int(65 <= a <= 90)),
        Fn("is_alpha", "ascii::is_alpha(a)", lambda a, b: int(a < 128 and chr(a).isalpha())),
        Fn("is_alnum", "ascii::is_alnum(a)", lambda a, b: int(a < 128 and chr(a).isalnum())),
        Fn("is_hex_digit", "ascii::is_hex_digit(a)", lambda a, b: int(a < 128 and chr(a) in "0123456789abcdefABCDEF")),
        Fn("is_space", "ascii::is_space(a)", lambda a, b: int(a in (32, 9, 10, 11, 12, 13))),
        Fn("is_control", "ascii::is_control(a)", lambda a, b: int(a < 32 or a == 127)),
        Fn("is_print", "ascii::is_print(a)", lambda a, b: int(32 <= a <= 126)),
        Fn("is_punct", "ascii::is_punct(a)", lambda a, b: int(a in PUNCT)),
        Fn("to_upper", "ascii::to_upper(a)", lambda a, b: a - 32 if 97 <= a <= 122 else a),
        Fn("to_lower", "ascii::to_lower(a)", lambda a, b: a + 32 if 65 <= a <= 90 else a),
        Fn("digit_value", "enc(ascii::digit_value(a))", lambda a, b: opt(a - 48 if 48 <= a <= 57 else None)),
        Fn("hex_value", "enc(ascii::hex_value(a))", lambda a, b: opt(int(chr(a), 16) if a < 128 and chr(a) in "0123456789abcdefABCDEF" else None)),
        Fn("hex_digit", "ascii::hex_digit(a as u32)", lambda a, b: ord("0123456789abcdef"[a & 15])),
    ]
    decls = "fn enc(o: Option<u32>) -> u32 { return match o { Option::Some(v) => v + 1, Option::None => 0 }; }\n"
    vals = list(range(256))
    prog = pair_program([("ascii", "std/ascii.nge")], "u8", vals, None, None, fs, decls)
    return [("ascii", {"main.nge": prog}, pair_expected(vals, None, fs))]


# ---------------------------------------------------------------------------------------------------------------
# str: words over a small alphabet, all pairs
# ---------------------------------------------------------------------------------------------------------------
STR_DECLS = """import "std/str.nge" as str;
"""

STR_HELPERS = """
fn hs<const N: usize>(ok: bool, s: str::Str<N>) -> u32 {
    var c = mix(mix(0, ok as u32), s.len as u32);
    var i: usize = 0;
    while i < s.len { c = mix(c, s.bytes[i] as u32); i = i + 1; }
    return c;
}
fn enc(o: Option<usize>) -> u32 { return match o { Option::Some(v) => v as u32 + 1, Option::None => 0 }; }
fn encb(o: Option<u8>) -> u32 { return match o { Option::Some(v) => v as u32 + 1, Option::None => 0 }; }
fn word(i: usize) -> str::Str<8> { return str::Str { bytes: W[i], len: WL[i] }; }
fn alpha_byte(i: usize) -> u8 { return ALPHA[i % 8]; }
fn t_append12(a: usize, b: usize) -> u32 {
    var x: str::Str<12> = str::new();
    let ok1 = str::append(&x, word(a));
    let h1 = hs(ok1, x);
    let ok2 = str::append(&x, word(b));
    return mix(h1, hs(ok2, x));
}
fn t_append6(a: usize, b: usize) -> u32 {
    var x: str::Str<6> = str::new();
    let ok1 = str::append(&x, word(a));
    let h1 = hs(ok1, x);
    let ok2 = str::append(&x, word(b));
    return mix(h1, hs(ok2, x));
}
fn t_bytes(a: usize, b: usize) -> u32 {
    var x: str::Str<10> = str::new();
    let ok = str::append_bytes(&x, W[a], WL[a]) && str::append_bytes(&x, W[b], WL[b]);
    return hs(ok, x);
}
fn t_slice(a: usize, b: usize) -> u32 {
    var from = b % 6;
    var to = b % 9;
    return match str::slice(word(a), from, to) { Option::Some(r) => hs(true, r), Option::None => 7 };
}
fn t_trim(a: usize) -> u32 { var w = word(a); str::trim(&w); return hs(true, w); }
fn t_upper(a: usize) -> u32 { var w = word(a); str::to_upper(&w); return hs(true, w); }
fn t_lower(a: usize) -> u32 { var w = word(a); str::to_lower(&w); return hs(true, w); }
fn t_reverse(a: usize) -> u32 { var w = word(a); str::reverse(&w); return hs(true, w); }
fn t_trunc(a: usize, b: usize) -> u32 { var w = word(a); str::truncate(&w, b % 9); return hs(true, w); }
fn t_edit(a: usize, b: usize) -> u32 {
    var w = word(a);
    let c = alpha_byte(b);
    let p1 = encb(str::pop(&w));
    let ok1 = str::push(&w, c);
    let ok2 = str::set(&w, b % 4, c);
    let g = encb(str::get(w, b % 9));
    return mix(mix(mix(mix(p1, ok1 as u32), ok2 as u32), g), hs(true, w));
}
fn t_term(a: usize) -> u32 {
    var w = word(a);
    let ok = str::terminate(&w);
    var z: u32 = 0;
    if ok { z = w.bytes[w.len] as u32 + 1; }
    return mix(ok as u32, z);
}
"""

ALPHA = [ord(c) for c in "aAbB 0\tz"]


def cs_hash(ok, data):
    c = Checksum()
    c.add(int(ok))
    c.add(len(data))
    for b in data:
        c.add(b)
    return c.acc


def mixv(acc, v):
    return (acc * 31 + (v & M32)) & M32


def suite_str(rng):
    words = [b"", b"a", b"ab", b"aba", b"ABA", b"  a ", b"a b", b"aaaa", b"abab", b"b", b"B0", b"\t\tx\t", b"        ", b"zzzzzzzz", b"0", b"abcabc"]
    for _ in range(50):
        n = rng.randint(0, 8)
        words.append(bytes(rng.choice(ALPHA) for _ in range(n)))
    K = len(words)
    padded = [list(w) + [rng.choice(ALPHA) for _ in range(8 - len(w))] for w in words]  # junk after the end: nothing may look at it
    decls = table("ALPHA", "u8", ALPHA) + "static W: [[u8; 8]; %d] = [%s];\n" % (K, ", ".join("[" + ", ".join(map(str, r)) + "]" for r in padded)) \
        + table("WL", "usize", [len(w) for w in words]) + STR_HELPERS

    def find(a, b): return a.find(b)
    def bset(w, i, c): return w[:i] + bytes([c]) + w[i + 1:]
    def trim(w):
        sp = b" \t\n\v\f\r"
        return w.strip(sp)
    def app(cap, a, b):
        x = b""
        ok1 = len(a) <= cap
        if ok1: x += a
        h1 = cs_hash(ok1, x)
        ok2 = len(b) <= cap - len(x)
        if ok2: x += b
        return mixv(h1, cs_hash(ok2, x)) if False else mixv(h1, cs_hash(ok2, x))
    def edit(w, b):
        x = bytearray(w)
        c = ALPHA[b % 8]
        p1 = (x.pop() + 1) if x else 0
        ok1 = len(x) < 8
        if ok1: x.append(c)
        i = b % 4
        ok2 = i < len(x)
        if ok2: x[i] = c
        g = b % 9
        gv = x[g] + 1 if g < len(x) else 0
        return mixv(mixv(mixv(mixv(p1, int(ok1)), int(ok2)), gv), cs_hash(True, bytes(x)))
    def slice_(w, b):
        f, t = b % 6, b % 9
        if f > t or t > len(w): return 7
        return cs_hash(True, w[f:t])

    ab = lambda b: ALPHA[b % 8]
    fs = [
        Fn("eq", "str::eq(word(a), word(b)) as u32", lambda a, b: int(words[a] == words[b])),
        Fn("compare", "str::compare(word(a), word(b)) as u32", lambda a, b: (-1 if words[a] < words[b] else 1 if words[a] > words[b] else 0) & M32),
        Fn("eq_ignore_case", "str::eq_ignore_case(word(a), word(b)) as u32", lambda a, b: int(words[a].lower() == words[b].lower())),
        Fn("starts_with", "str::starts_with(word(a), word(b)) as u32", lambda a, b: int(words[a].startswith(words[b]))),
        Fn("ends_with", "str::ends_with(word(a), word(b)) as u32", lambda a, b: int(words[a].endswith(words[b]))),
        Fn("find", "enc(str::find(word(a), word(b)))", lambda a, b: find(words[a], words[b]) + 1 if find(words[a], words[b]) >= 0 else 0),
        Fn("contains", "str::contains(word(a), word(b)) as u32", lambda a, b: int(words[b] in words[a])),
        Fn("find_byte", "enc(str::find_byte(word(a), alpha_byte(b)))", lambda a, b: words[a].find(bytes([ab(b)])) + 1),
        Fn("count_byte", "str::count_byte(word(a), alpha_byte(b)) as u32", lambda a, b: words[a].count(bytes([ab(b)]))),
        Fn("append12", "t_append12(a, b)", lambda a, b: app(12, words[a], words[b])),
        Fn("append6", "t_append6(a, b)", lambda a, b: app(6, words[a], words[b])),
        Fn("append_bytes", "t_bytes(a, b)", lambda a, b: cs_hash(len(words[a]) + len(words[b]) <= 10, (words[a] + words[b]) if len(words[a]) + len(words[b]) <= 10 else words[a] if len(words[a]) <= 10 else b"")),
        Fn("slice", "t_slice(a, b)", lambda a, b: slice_(words[a], b)),
        Fn("trim", "t_trim(a)", lambda a, b: cs_hash(True, trim(words[a]))),
        Fn("upper", "t_upper(a)", lambda a, b: cs_hash(True, bytes(c - 32 if 97 <= c <= 122 else c for c in words[a]))),
        Fn("lower", "t_lower(a)", lambda a, b: cs_hash(True, bytes(c + 32 if 65 <= c <= 90 else c for c in words[a]))),
        Fn("reverse", "t_reverse(a)", lambda a, b: cs_hash(True, words[a][::-1])),
        Fn("truncate", "t_trunc(a, b)", lambda a, b: cs_hash(True, words[a][:b % 9])),
        Fn("edit", "t_edit(a, b)", lambda a, b: edit(words[a], b)),
        Fn("terminate", "t_term(a)", lambda a, b: mixv(int(len(words[a]) < 8), (1 if len(words[a]) < 8 else 0))),
    ]
    # `append_bytes` into a Str<10>: both appends must fit or the second leaves the string as it was after the first
    idx = list(range(K))
    prog = pair_program([("str", "std/str.nge")], "usize", idx, "usize", idx, fs, decls)
    # t_bytes: ok = append(a) && append(b): when a fits (always: <= 8 <= 10) and b does not fit afterwards, the string holds a
    return [("str_words", {"main.nge": prog}, pair_expected(idx, idx, fs))]


def suite_cstr(rng):
    """The C-string functions of std/str.nge: append_cstr and from_cstr, with literals of several lengths into strings
    of several capacities that already hold some bytes."""
    literals = ["", "a", "abc", "hello", "12345678", "abcdefghij", "x y\tz", "\\n"]
    lit_bytes = [b"", b"a", b"abc", b"hello", b"12345678", b"abcdefghij", b"x y\tz", b"\n"]
    caps = (1, 3, 5, 8, 12)
    src = """import "std/str.nge" as str;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn hs<const N: usize>(ok: bool, s: str::Str<N>) -> u32 {
    var c = mix(mix(0, ok as u32), s.len as u32);
    var i: usize = 0;
    while i < s.len { c = mix(c, s.bytes[i] as u32); i = i + 1; }
    return c;
}
"""
    for cap in caps:
        src += """fn app%(n)d(p: *const u8, pre: usize) -> u32 {
    var s: str::Str<%(n)d> = str::new();
    var k: usize = 0;
    while k < pre { str::push(&s, 'x'); k = k + 1; }
    var ok = false;
    unsafe { ok = str::append_cstr(&s, p); }
    return hs(ok, s);
}
fn from%(n)d(p: *const u8) -> u32 {
    var r: Option<str::Str<%(n)d>> = Option::None;
    unsafe { r = str::from_cstr(p); }
    return match r { Option::Some(s) => hs(true, s), Option::None => 99 };
}
""" % {"n": cap}
    src += "\nfn main() -> i32 {\n"
    out = ""
    for cap in caps:
        a = Checksum()
        f = Checksum()
        src += "    var a%d: u32 = 0;\n    var f%d: u32 = 0;\n" % (cap, cap)
        for lit, lb in zip(literals, lit_bytes):
            for pre in (0, 1, 2, 4):
                if pre > cap:
                    continue
                src += '    a%d = mix(a%d, app%d(c"%s", %d));\n' % (cap, cap, cap, lit, pre)
                ok = len(lb) <= cap - pre
                a.add(cs_hash(ok, (b"x" * pre + lb) if ok else b"x" * pre))
            src += '    f%d = mix(f%d, from%d(c"%s"));\n' % (cap, cap, cap, lit)
            f.add(cs_hash(True, lb) if len(lb) <= cap else 99)
        src += "    print(a%d as i32);\n    print(f%d as i32);\n" % (cap, cap)
        out += "%d\n%d\n" % (a.value(), f.value())
    src += "    return 0;\n}\n"
    return [("cstr", {"main.nge": src}, out)]


# ---------------------------------------------------------------------------------------------------------------
# fmt: numbers to text and text to numbers
# ---------------------------------------------------------------------------------------------------------------
def fmt_int_padded(x, w, pad):
    digits_ = str(abs(x))
    sign = "-" if x < 0 else ""
    fill = max(0, w - len(digits_) - len(sign))
    if pad == "0":
        return sign + "0" * fill + digits_
    return pad * fill + sign + digits_


def fmt_fixed(x, d):
    if d > 19:
        return None
    mag = abs(x)
    scale = 10 ** d
    t = ("-" if x < 0 else "") + str(mag // scale)
    if d > 0:
        t += "." + str(mag % scale).zfill(d)
    return t


def parse_digits_model(text, frm, base):
    if frm >= len(text):
        return ("err", 0)
    acc = 0
    for ch in text[frm:]:
        v = "0123456789abcdefABCDEF".find(chr(ch))
        if v < 0:
            return ("err", 1)
        if v > 15:
            v -= 6
        if v >= base:
            return ("err", 1)
        acc = acc * base + v
        if acc > M64:
            return ("err", 2)
    return ("ok", acc)


def parse_int_model(text):
    if len(text) == 0:
        return ("err", 0)
    neg = False
    frm = 0
    if text[0] == ord("-"):
        neg, frm = True, 1
    elif text[0] == ord("+"):
        frm = 1
    if frm == 1 and len(text) == 1:
        return ("err", 1)
    kind, v = parse_digits_model(text, frm, 10)
    if kind == "err":
        return (kind, v)
    if neg:
        return ("err", 2) if v > 1 << 63 else ("ok", (-v) & M64)
    return ("err", 2) if v > (1 << 63) - 1 else ("ok", v)


def suite_fmt(rng):
    u64s = sorted({0, 1, 9, 10, 11, 99, 100, 255, 256, 999, 1000, 65535, 65536, 2**31 - 1, 2**31, 2**32 - 1, 2**32, 10**18, 10**19 - 1, 10**19, 2**63 - 1, 2**63, M64 - 1, M64} | {rng.getrandbits(rng.randint(1, 64)) for _ in range(40)})
    i64s = sorted({0, 1, -1, 9, -9, 10, -10, 99, -99, 100, 12345, -12345, 2**31 - 1, -2**31, 2**63 - 1, -2**63, -2**63 + 1, 5, -5, 50, -50, 1000000, -1000000}
                  | {rng.randint(-2**63, 2**63 - 1) >> rng.randint(0, 62) for _ in range(40)})
    widths = [0, 1, 3, 8, 25]
    pads = [ord(" "), ord("0"), ord("*")]
    helpers = """import "std/str.nge" as str;
import "std/fmt.nge" as fmt;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn hs<const N: usize>(ok: bool, s: str::Str<N>) -> u32 {
    var c = mix(mix(0, ok as u32), s.len as u32);
    var i: usize = 0;
    while i < s.len { c = mix(c, s.bytes[i] as u32); i = i + 1; }
    return c;
}
fn enc_u(r: Result<u64, fmt::ParseError>) -> u32 {
    return match r {
        Result::Ok(v) => mix(mix(1, v as u32), (v >> 32) as u32),
        Result::Err(e) => mix(2, e as u32),
    };
}
fn enc_i(r: Result<i64, fmt::ParseError>) -> u32 {
    return match r {
        Result::Ok(v) => mix(mix(1, v as u32), ((v as u64) >> 32) as u32),
        Result::Err(e) => mix(2, e as u32),
    };
}
"""
    # --- writing: each case is (capacity, a call writing x [and w/pad], model text) -------------------------------
    texts = {}
    cases = []
    for cap in (40, 5):
        for x in u64s:
            cases.append(("uint%d" % cap, cap, "fmt::append_uint(&s, X)", x, None, str(x)))
        for x in i64s:
            cases.append(("int%d" % cap, cap, "fmt::append_int(&s, X)", x, None, str(x)))
    for x in u64s[::3]:
        for w in widths:
            for p in pads:
                cases.append(("uintpad", 40, "fmt::append_uint_padded(&s, X, %d, %d)" % (w, p), x, None, chr(p) * max(0, w - len(str(x))) + str(x)))
    for x in i64s[::3]:
        for w in widths:
            for p in pads:
                cases.append(("intpad", 40, "fmt::append_int_padded(&s, X, %d, %d)" % (w, p), x, None, fmt_int_padded(x, w, chr(p))))
    for x in u64s[::2]:
        for d in (0, 1, 2, 8, 16, 20):
            cases.append(("hex", 40, "fmt::append_hex(&s, X, %d)" % d, x, None, format(x, "x").rjust(d, "0")))
        for d in (0, 1, 8, 64):
            cases.append(("bin", 80, "fmt::append_bin(&s, X, %d)" % d, x, None, format(x, "b").rjust(d, "0")))
    for x in i64s:
        for d in (0, 1, 2, 3, 5, 9, 19, 20):
            cases.append(("fixed", 48, "fmt::append_fixed(&s, X, %d)" % d, x, None, fmt_fixed(x, d)))
    for x in u64s[::4]:
        for w in (3, 8, 25):
            cases.append(("uintpad6", 6, "fmt::append_uint_padded(&s, X, %d, 32)" % w, x, None, " " * max(0, w - len(str(x))) + str(x)))
    for x in i64s[::4]:
        for w in (3, 8, 25):
            cases.append(("intpad6", 6, "fmt::append_int_padded(&s, X, %d, 48)" % w, x, None, fmt_int_padded(x, w, "0")))
    for x in (0, 1, -1, 5, -5, 12, -12, 123, -123, 1234, -1234, 12345, 99999, -99999, 100000, 7):
        for d in (0, 1, 2, 3, 4, 5):
            for cap in (4, 6, 8):
                cases.append(("fixedtight%d" % cap, cap, "fmt::append_fixed(&s, X, %d)" % d, x, None, fmt_fixed(x, d)))
    # one function per capacity keeps the program small: a table of values per kind, a loop over it
    body = ""
    out_cks = []
    groups = {}
    for kind, cap, call, x, _, text in cases:
        groups.setdefault((kind, cap, call.split("(&s, X")[0], call.split("X", 1)[1]), []).append((x, text))
    decls = ""
    mains = []
    gi = 0
    for (kind, cap, fn, tail), items in groups.items():
        gi += 1
        is_signed = fn.endswith("::append_int") or fn.endswith("::append_int_padded") or fn.endswith("::append_fixed")
        ty = "i64" if is_signed else "u64"
        decls += table("G%d" % gi, ty, [x for x, _ in items])
        decls += "fn run%d(x: %s) -> u32 {\n    var s: str::Str<%d> = str::new();\n    let ok = %s(&s, x%s;\n    return hs(ok, s);\n}\n" % (gi, ty, cap, fn, tail)
        mains.append("    var c%d: u32 = 0;\n    i = 0;\n    while i < %d { c%d = mix(c%d, run%d(G%d[i])); i = i + 1; }\n    print(c%d as i32);\n" % (gi, len(items), gi, gi, gi, gi, gi))
    # --- reading ----------------------------------------------------------------------------------------------------
    pstr = [b"", b"0", b"7", b"00012", b"+5", b"-5", b"-", b"+", b"12a", b"a12", b" 12", b"12 ", b"1 2", b"--1", b"+-1",
            b"18446744073709551615", b"18446744073709551616", b"-9223372036854775808", b"-9223372036854775809",
            b"9223372036854775807", b"9223372036854775808", b"99999999999999999999x", b"1x99999999999999999999",
            b"ff", b"FF", b"0x1f", b"g", b"ffffffffffffffff", b"10000000000000000", b"1011", b"102", b"-0", b"+0", b"000000000000000000000000",
            b"4294967296", b"-2147483648", b"2147483648", b"deadBEEF", b"7fffffffffffffff", b"8000000000000000"]
    for _ in range(80):
        n = rng.randint(1, 22)
        alphabet = b"0123456789" if rng.random() < 0.7 else b"0123456789abcdefABCDEFxg+- "
        pstr.append(bytes(rng.choice(alphabet) for _ in range(n)))
    padded = [list(t) + [0] * (24 - len(t)) for t in pstr]
    decls += "static PS: [[u8; 24]; %d] = [%s];\n" % (len(pstr), ", ".join("[" + ", ".join(map(str, r)) + "]" for r in padded))
    decls += table("PL", "usize", [len(t) for t in pstr])
    decls += "fn ps(i: usize) -> str::Str<24> { return str::Str { bytes: PS[i], len: PL[i] }; }\n"
    kinds = [("parse_uint", "enc_u", lambda t: parse_digits_model(t, 0, 10)), ("parse_hex", "enc_u", lambda t: parse_digits_model(t, 0, 16)),
             ("parse_bin", "enc_u", lambda t: parse_digits_model(t, 0, 2)), ("parse_int", "enc_i", parse_int_model)]
    decls += table("GB", "u64", [0, 1])
    for cap in (4, 5, 40):
        decls += "fn runb%d(x: u64) -> u32 {\n    var s: str::Str<%d> = str::new();\n    let ok = fmt::append_bool(&s, x == 1);\n    return hs(ok, s);\n}\n" % (cap, cap)
        mains.append("    var cb%d: u32 = 0;\n    i = 0;\n    while i < 2 { cb%d = mix(cb%d, runb%d(GB[i])); i = i + 1; }\n    print(cb%d as i32);\n" % (cap, cap, cap, cap, cap))
    for fn, enc, model in kinds:
        gi += 1
        mains.append("    var c%d: u32 = 0;\n    i = 0;\n    while i < %d { c%d = mix(c%d, %s(fmt::%s(ps(i)))); i = i + 1; }\n    print(c%d as i32);\n" % (gi, len(pstr), gi, gi, enc, fn, gi))
    prog = helpers + decls + "\nfn main() -> i32 {\n    var i: usize = 0;\n" + "".join(mains) + "    return 0;\n}\n"
    # --- the expected output, same order -------------------------------------------------------------------------------
    out = ""
    for (kind, cap, fn, tail), items in groups.items():
        c = 0
        for x, text in items:
            if text is None or len(text) > cap:
                v = cs_hash(False, b"")
            else:
                v = cs_hash(True, text.encode())
            c = mixv(c, v)
        out += "%d\n" % s32(c)
    for cap in (4, 5, 40):
        c = 0
        for text in ("false", "true"):
            c = mixv(c, cs_hash(True, text.encode()) if len(text) <= cap else cs_hash(False, b""))
        out += "%d\n" % s32(c)
    for fn, enc, model in kinds:
        c = 0
        for t in pstr:
            kind, v = model(t)
            if kind == "ok":
                if enc == "enc_u":
                    r = mixv(mixv(1, v & M32), (v >> 32) & M32)
                else:
                    r = mixv(mixv(1, v & M32), (v >> 32) & M32)
            else:
                r = mixv(2, v)
            c = mixv(c, r)
        out += "%d\n" % s32(c)
    return [("fmt", {"main.nge": prog}, out)]


BITSET_OPS = [("set", "i", ""), ("clear", "i", ""), ("toggle", "i", ""), ("put", "i", "v"), ("test", "i", ""), ("count", "", ""),
              ("is_empty", "", ""), ("next_set", "f", ""), ("next_clear", "f", ""), ("first_set", "", ""), ("first_clear", "", ""),
              ("set_all", "", ""), ("clear_all", "", ""), ("union", "", "p"), ("intersect", "", "p"), ("difference", "", "p"),
              ("xor", "", "p"), ("equal", "", "p"), ("subset", "", "p")]
BITSET_WEIGHTS = [10, 7, 4, 4, 6, 2, 1, 4, 4, 2, 2, 0.4, 0.4, 2, 2, 2, 2, 2, 3]


def bitset_ops(rng, W, count):
    ops = []
    for _ in range(count):
        c = rng.choices(range(len(BITSET_OPS)), BITSET_WEIGHTS)[0]
        kind_a, kind_b = BITSET_OPS[c][1], BITSET_OPS[c][2]
        a = rng.randrange(32 * W) if kind_a == "i" else rng.randrange(32 * W + 4) if kind_a == "f" else 0
        if kind_b == "v":
            b = rng.randrange(2)
        elif kind_b == "p":
            r = rng.random()
            b = rng.getrandbits(32) if r < 0.4 else (rng.getrandbits(32) & rng.getrandbits(32) & rng.getrandbits(32)) if r < 0.8 else (M32 if r < 0.9 else 0)
            b = s32(b)
        else:
            b = 0
        ops.append((c, a, b))
    return ops


def bitset_program(W, ops):
    src = """import "std/bitset.nge" as bitset;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn mix_idx(acc: u32, o: Option<usize>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
""" + ops_tables(ops)
    src += """
fn pattern(pat: u32) -> bitset::Bitset<%(W)d> {
    var other: bitset::Bitset<%(W)d> = bitset::new();
    var k: usize = 0;
    while k < %(W)d {
        other.words[k] = if_odd(k, pat);
        k = k + 1;
    }
    return other;
}
fn if_odd(k: usize, pat: u32) -> u32 {
    if k %% 2 == 1 { return ~pat; }
    return pat;
}

fn main() -> i32 {
    var b: bitset::Bitset<%(W)d> = bitset::new();
    var c: u32 = 0;
    var k: usize = 0;
    while k < %(n)d {
        let op = OPC[k];
        let a = OPA[k] as usize;
        let pat = OPB[k] as u32;
""" % {"W": W, "n": len(ops)}
    chain = [
        "bitset::set(&b, a);",
        "bitset::clear(&b, a);",
        "bitset::toggle(&b, a);",
        "bitset::put(&b, a, pat == 1);",
        "c = mix(c, bitset::test(b, a) as u32);",
        "c = mix(c, bitset::count(b) as u32);",
        "c = mix(c, bitset::is_empty(b) as u32);",
        "c = mix_idx(c, bitset::next_set(b, a));",
        "c = mix_idx(c, bitset::next_clear(b, a));",
        "c = mix_idx(c, bitset::first_set(b));",
        "c = mix_idx(c, bitset::first_clear(b));",
        "bitset::set_all(&b);",
        "bitset::clear_all(&b);",
        "bitset::union_with(&b, pattern(pat));",
        "bitset::intersect_with(&b, pattern(pat));",
        "bitset::difference_with(&b, pattern(pat));",
        "bitset::xor_with(&b, pattern(pat));",
        "c = mix(c, bitset::equal(b, pattern(pat)) as u32);",
        "c = mix(mix(c, bitset::is_subset(b, pattern(pat)) as u32), bitset::is_subset(pattern(pat), b) as u32);",
    ]
    for i, text in enumerate(chain):
        src += "        %sif op == %d { %s }\n" % ("" if i == 0 else "else ", i, text)
    src += """        var w: usize = 0;
        while w < %d { c = mix(c, b.words[w]); w = w + 1; }
        k = k + 1;
    }
    print(c as i32);
    print(bitset::capacity(b) as i32);
    return 0;
}
""" % W
    return src


def bitset_expected(W, ops):
    full = (1 << (32 * W)) - 1
    def pat(p):
        v = 0
        for k in range(W):
            word = (p & M32) if k % 2 == 0 else (~p & M32)
            v |= word << (32 * k)
        return v
    st = 0
    m = Mix()
    for op, a, b in ops:
        if op == 0: st |= 1 << a
        elif op == 1: st &= ~(1 << a)
        elif op == 2: st ^= 1 << a
        elif op == 3:
            if b == 1: st |= 1 << a
            else: st &= ~(1 << a)
        elif op == 4: m((st >> a) & 1)
        elif op == 5: m(bin(st).count("1"))
        elif op == 6: m(int(st == 0))
        elif op in (7, 8, 9, 10):
            frm = a if op in (7, 8) else 0
            target = 1 if op in (7, 9) else 0
            found = None
            for i in range(frm, 32 * W):
                if (st >> i) & 1 == target:
                    found = i
                    break
            m.opt(found)
        elif op == 11: st = full
        elif op == 12: st = 0
        elif op == 13: st |= pat(b)
        elif op == 14: st &= pat(b)
        elif op == 15: st &= ~pat(b) & full
        elif op == 16: st ^= pat(b)
        elif op == 17: m(int(st == pat(b)))
        elif op == 18:
            m(int(st & ~pat(b) == 0))
            m(int(pat(b) & ~st == 0))
        for w in range(W):
            m((st >> (32 * w)) & M32)
    return "%d\n%d\n" % (m.value(), 32 * W)


def suite_bitset(rng):
    jobs = []
    for W in (1, 2, 3):
        ops = bitset_ops(rng, W, 3000)
        jobs.append(("bitset_%d" % W, {"main.nge": bitset_program(W, ops)}, bitset_expected(W, ops)))
    return jobs


MAP_OPS = [("put", "k", "v"), ("get", "k", ""), ("has", "k", ""), ("remove", "k", ""), ("get_or", "k", "v"), ("key_at", "i", ""),
           ("value_at", "i", ""), ("index_of", "k", ""), ("clear", "", ""), ("flags", "", "")]
MAP_WEIGHTS = [12, 6, 4, 6, 3, 3, 3, 3, 0.3, 1]


def map_ops(rng, N, count):
    ops = []
    for _ in range(count):
        c = rng.choices(range(len(MAP_OPS)), MAP_WEIGHTS)[0]
        def arg(kind):
            return rng.randrange(N + 4) if kind == "k" else rng.randint(0, N + 2) if kind == "i" else rng.randint(-50, 50) if kind == "v" else 0
        ops.append((c, arg(MAP_OPS[c][1]), arg(MAP_OPS[c][2])))
    return ops


def map_program(N, ops):
    src = """import "std/map.nge" as map;

fn mix(acc: u32, v: u32) -> u32 { return wrapping_add(wrapping_mul(acc, 31), v); }
fn mix_opt(acc: u32, o: Option<i32>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
fn mix_key(acc: u32, o: Option<u8>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
fn mix_idx(acc: u32, o: Option<usize>) -> u32 {
    return match o { Option::Some(v) => mix(mix(acc, 1), v as u32), Option::None => mix(acc, 0) };
}
""" + ops_tables(ops)
    src += """
fn main() -> i32 {
    var m: map::Map<u8, i32, %(N)d> = map::new(255, -1);
    var c: u32 = 0;
    var k: usize = 0;
    while k < %(n)d {
        let op = OPC[k];
        let key = OPA[k] as u8;
        let a = OPA[k] as usize;
        let val = OPB[k];
""" % {"N": N, "n": len(ops)}
    chain = [
        "c = mix(c, map::put(&m, key, val) as u32);",
        "c = mix_opt(c, map::get(m, key));",
        "c = mix(c, map::has(m, key) as u32);",
        "c = mix_opt(c, map::remove(&m, key));",
        "c = mix(c, map::get_or(m, key, val) as u32);",
        "c = mix_key(c, map::key_at(m, a));",
        "c = mix_opt(c, map::value_at(m, a));",
        "c = mix_idx(c, map::index_of(m, key));",
        "map::clear(&m);",
        "c = mix(mix(mix(mix(c, map::is_full(m) as u32), map::is_empty(m) as u32), map::capacity(m) as u32), map::size(m) as u32);",
    ]
    for i, text in enumerate(chain):
        src += "        %sif op == %d { %s }\n" % ("" if i == 0 else "else ", i, text)
    src += """        var i: usize = 0;
        while i < map::size(m) { c = mix_key(c, map::key_at(m, i)); c = mix_opt(c, map::value_at(m, i)); i = i + 1; }
        k = k + 1;
    }
    print(c as i32);
    return 0;
}
"""
    return src


def map_expected(N, ops):
    pairs = []
    m = Mix()
    def find(k):
        for i, (kk, _) in enumerate(pairs):
            if kk == k: return i
        return None
    for op, a, b in ops:
        key = a & 255
        i = find(key)
        if op == 0:
            if i is not None:
                pairs[i][1] = b
                m(1)
            elif len(pairs) < N:
                pairs.append([key, b])
                m(1)
            else:
                m(0)
        elif op == 1: m.opt(pairs[i][1] if i is not None else None)
        elif op == 2: m(int(i is not None))
        elif op == 3:
            if i is not None:
                v = pairs[i][1]
                pairs[i] = pairs[-1]
                pairs.pop()
                m.opt(v)
            else:
                m.opt(None)
        elif op == 4: m(pairs[i][1] if i is not None else b)
        elif op == 5: m.opt(pairs[a][0] if a < len(pairs) else None)
        elif op == 6: m.opt(pairs[a][1] if a < len(pairs) else None)
        elif op == 7: m.opt(i)
        elif op == 8: pairs.clear()
        elif op == 9:
            m(int(len(pairs) == N)); m(int(len(pairs) == 0)); m(N); m(len(pairs))
        for kk, vv in pairs:
            m(1); m(kk); m(1); m(vv)
    return "%d\n" % m.value()


def suite_map(rng):
    jobs = []
    for N in (1, 3, 8, 16):
        ops = map_ops(rng, N, 3000)
        jobs.append(("map_%d" % N, {"main.nge": map_program(N, ops)}, map_expected(N, ops)))
    return jobs


# ---------------------------------------------------------------------------------------------------------------
# the driver
# ---------------------------------------------------------------------------------------------------------------
SUITES = {
    "math": suite_math,
    "bits": suite_bits,
    "array": suite_array,
    "vec": suite_vec,
    "ring": suite_ring,
    "rand": suite_rand,
    "hash": suite_hash,
    "ascii": suite_ascii,
    "str": suite_str,
    "cstr": suite_cstr,
    "fmt": suite_fmt,
    "bitset": suite_bitset,
    "map": suite_map,
}


def run_job(job):
    name, files, want = job
    try:
        got = build_and_run(name, files)
    except Exception as e:
        return (name, "FAILED: %s" % e)
    if got != want:
        gl, wl = got.split("\n"), want.split("\n")
        for k in range(max(len(gl), len(wl))):
            g = gl[k] if k < len(gl) else "<missing>"
            w = wl[k] if k < len(wl) else "<missing>"
            if g != w:
                return (name, "WRONG at output line %d: the library printed %s, the model says %s" % (k + 1, g, w))
        return (name, "WRONG output")
    return (name, None)


def main():
    names = [n for n in SUITES if not ONLY or n in ONLY.split(",")]
    jobs = []
    for n in names:
        jobs.extend(SUITES[n](random.Random(SEED * 1000003 + sum(map(ord, n)))))
    bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for name, problem in pool.map(run_job, jobs):
            if problem:
                bad += 1
                print("%s: %s" % (name, problem))
    print("%d programs checked against the models: %s" % (len(jobs), "all agree" if not bad else "%d PROBLEMS" % bad))
    sys.exit(1 if bad else 0)


main()
