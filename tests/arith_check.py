#!/usr/bin/env python3
"""Differential test of the generated overflow / division / shift checks.

For every integer type and operator it compiles a one-operator Noge function, builds the generated C
with NOGE_TRAP redefined to longjmp, and compares each result (or trap kind) against __int128
reference arithmetic over edge values plus pseudo-random pairs. Runs the default and --no-builtins
code paths. Needs python3 and a C compiler with __int128 (gcc/clang on a 64-bit host).

usage: tests/arith_check.py [path/to/nogec]        (also: make check-arith)
"""
import os, subprocess, sys, tempfile

NOGEC = sys.argv[1] if len(sys.argv) > 1 else "build/nogec"
CC = os.environ.get("CC", "cc")
TYPES = {  # name: (bits, signed, C type)
    "i8": (8, True, "int8_t"), "i16": (16, True, "int16_t"), "i32": (32, True, "int32_t"), "i64": (64, True, "int64_t"),
    "u8": (8, False, "uint8_t"), "u16": (16, False, "uint16_t"), "u32": (32, False, "uint32_t"), "u64": (64, False, "uint64_t"),
}
OVERFLOW, DIVZERO, SHIFT = 1, 2, 4

def values(bits, signed):
    lo = -(1 << (bits - 1)) if signed else 0
    hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
    edge = {0, 1, 2, 3, 7, lo, lo + 1, lo + 2, hi, hi - 1, hi - 2, hi // 2, hi // 2 + 1, lo // 2, -1, -2, 1 << (bits // 2), (1 << (bits // 2)) - 1, 10, 100}
    edge |= {(1 << k) for k in range(bits - (1 if signed else 0))} | {(1 << k) - 1 for k in range(1, bits - (1 if signed else 0))}
    vals = sorted(v for v in edge if lo <= v <= hi)
    seed, rnd = 12345, set()
    while len(rnd) < 40:
        seed = (seed * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        v = (seed >> 8) % (1 << bits)
        rnd.add(v - (1 << bits) if signed and v >= (1 << (bits - 1)) else v)
    return vals, sorted(rnd)

OPS = [  # name, noge expression, arity
    ("add", "a + b", 2), ("sub", "a - b", 2), ("mul", "a * b", 2), ("div", "a / b", 2), ("rem", "a % b", 2),
    ("shl", "a << b", 2), ("shr", "a >> b", 2), ("band", "a & b", 2), ("bor", "a | b", 2), ("bxor", "a ^ b", 2),
    ("wadd", "wrapping_add(a, b)", 2), ("wsub", "wrapping_sub(a, b)", 2), ("wmul", "wrapping_mul(a, b)", 2),
    ("neg", "-a", 1), ("bnot", "~a", 1),
]

def driver(tname, bits, signed, ctype, op, vals):
    lo = -(1 << (bits - 1)) if signed else 0
    hi = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
    n = len(vals)
    arr = ", ".join(f"({ctype}){v}{'LL' if signed else 'ULL'}" if v != lo or not signed or bits < 64 else "INT64_MIN" for v in vals)
    return f"""
#include <setjmp.h>
static jmp_buf jb_;
#define NOGE_TRAP(kind, line) longjmp(jb_, (kind))
#define main generated_main_
#include "gen.c"
#undef main
typedef __int128 S; typedef unsigned __int128 U;
static const {ctype} V[] = {{ {arr} }};
static S wrap(S z) {{ U m = (((U)1) << {bits}) - 1; U u = ((U)z) & m; S r = (S)u; if ({'1' if signed else '0'} && u >= (((U)1) << {bits - 1})) r -= (((S)1) << {bits}); return r; }}
int main(void) {{
    int bad = 0, trapped = 0;
    for (int i = 0; i < {n}; i++) for (int j = 0; j < {n}; j++) {{
        S x = V[i], y = V[j], want = 0; int want_trap = 0, got_trap, k;
        S LO = {lo}LL, HI = {hi}ULL; (void)LO; (void)HI; (void)y;
        {expected(op, bits, signed)}
        k = setjmp(jb_);
        S got = 0;
        if (k == 0) {{ got = ({"f_op(V[i], V[j])" if OPARITY[op] == 2 else "f_op(V[i])"}); got_trap = 0; }} else got_trap = k;
        if (got_trap != want_trap || (!want_trap && got != want)) {{
            if (bad++ < 5) printf("MISMATCH {tname} {op} a=%lld b=%lld: want %s%lld got %s%lld\\n", (long long)x, (long long)y,
                                  want_trap ? "trap#" : "", want_trap ? want_trap : (long long)want, got_trap ? "trap#" : "", got_trap ? got_trap : (long long)got);
        }}
        trapped += got_trap != 0;
    }}
    printf("%s %d pairs, %d traps, %d bad\\n", "{tname} {op}", {n * n}, trapped, bad);
    return bad != 0;
}}
"""

OPARITY = {name: arity for name, _, arity in OPS}

def expected(op, bits, signed):
    rng = "want_trap = (want < LO || want > HI) ? 1 : 0;"
    if op == "mul" and not signed and bits == 64:  # the product can exceed signed __int128
        return "{ U p = (U)x * (U)y; want_trap = p > (U)HI ? 1 : 0; want = (S)p; }"
    if op in ("add", "sub", "mul"):
        sym = {"add": "+", "sub": "-", "mul": "*"}[op]
        return f"want = x {sym} y; {rng}"
    if op in ("div", "rem"):
        sym = "/" if op == "div" else "%"
        return f"if (y == 0) want_trap = 2; else if ({'1' if signed else '0'} && x == LO && y == -1) want_trap = 1; else want = x {sym} y;"
    if op in ("shl", "shr"):
        body = (f"want = wrap((S)(((U)x & ((((U)1) << {bits}) - 1)) << (int)y));" if op == "shl" else "want = x >> (int)y;")
        return f"if (y < 0 || y >= {bits}) want_trap = 4; else {{ {body} }}"
    if op in ("band", "bor", "bxor"):
        sym = {"band": "&", "bor": "|", "bxor": "^"}[op]
        return f"want = wrap(x {sym} y);"
    if op == "wmul":  # modular: multiply as unsigned so a huge product wraps instead of overflowing
        return "want = wrap((S)((U)x * (U)y));"
    if op in ("wadd", "wsub"):
        sym = {"wadd": "+", "wsub": "-"}[op]
        return f"want = wrap(x {sym} y);"
    if op == "neg":
        return "if (x == LO) want_trap = 1; else want = -x;"
    return "want = wrap(~x);"  # bnot

def main():
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        for tname, (bits, signed, ctype) in TYPES.items():
            vals = sorted(set(sum(values(bits, signed), [])))
            for op, expr, arity in OPS:
                if op == "neg" and not signed:
                    continue
                params = f"a: {tname}, b: {tname}" if arity == 2 else f"a: {tname}"
                src = f"fn op({params}) -> {tname} {{ return {expr}; }}\nfn main() -> i32 {{ return 0; }}\n"
                open(f"{tmp}/p.nge", "w").write(src)
                for mode in ([], ["--no-builtins"]):
                    subprocess.run([NOGEC, *mode, "-o", f"{tmp}/gen.c", f"{tmp}/p.nge"], check=True)
                    open(f"{tmp}/drv.c", "w").write("#include <stdio.h>\n" + driver(tname, bits, signed, ctype, op, vals))
                    r = subprocess.run([CC, "-std=gnu99", "-O1", "-w", "-I", tmp, "-o", f"{tmp}/drv", f"{tmp}/drv.c"], capture_output=True, text=True)
                    if r.returncode:
                        print(r.stderr[:2000]); sys.exit(2)
                    r = subprocess.run([f"{tmp}/drv"], capture_output=True, text=True)
                    line = (r.stdout.strip().splitlines() or ["(no output)"])
                    if r.returncode:
                        bad += 1
                        print(f"[{'portable' if mode else 'builtin'}]", *line, sep="\n  ")
    print("arith_check:", "FAILED" if bad else "ok", f"({len(TYPES)} types x {len(OPS)} operators x 2 modes)")
    sys.exit(1 if bad else 0)

main()
