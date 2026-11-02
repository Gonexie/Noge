#!/usr/bin/env python3
"""Differential tests of enums and match against models written independently of the compiler.

Random enums (nested, with bool, integer and enum values) and random lists of match arms (variant,
literal, name, wildcard and '|' patterns, nested up to three deep) are generated. For each one:

1. Verdict. The compiler's verdict (accepted / "not exhaustive" / "can never match") is compared with
   a brute-force one: every value of the scrutinee's type is enumerated (an integer ranges over the
   literals the patterns use plus one value that none of them names) and tried against the arms.
2. Meaning. For every accepted program the generated C is built with -Wall -Wextra -Werror and run on a
   sample of values; each result, which encodes the arm taken and the values its names are bound to,
   is compared with a small interpreter of match semantics (first arm that fits wins).

usage: tests/match_check.py [path/to/nogec] [--cases N] [--seed S]        (also: make check-match)
"""
import itertools, os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

args = [a for a in sys.argv[1:]]
def option(name, default):
    if name in args:
        i = args.index(name)
        value = int(args[i + 1])
        del args[i:i + 2]
        return value
    return default
CASES = option("--cases", 1500)
SEED = option("--seed", 1)
NOGEC = args[0] if args else "build/nogec"
CC = os.environ.get("CC", "cc").split()      # may carry flags, for example CC="gcc -O2"
# to try another C compiler, for example tcc:  CC=tcc CFLAGS_WARN= NOGEC_FLAGS=--no-builtins python3 tests/match_check.py
CFLAGS_WARN = os.environ.get("CFLAGS_WARN", "-std=c99 -Wall -Wextra -Wpedantic -Werror").split()
NOGEC_FLAGS = os.environ.get("NOGEC_FLAGS", "").split()
INT_VALUES = [-1, 0, 1, 2, 99]       # the literals patterns may use are -1, 0, 1, 2; 99 stands for "any other number"
INT_LITERALS = [-1, 0, 1, 2]


DISCRIMINANTS = [-5000000000, -5, -1, 0, 1, 2, 7, 100, 255, 256, 1000, 70000, 3000000000, 5000000000]


class Enum:
    def __init__(self, name, variants, explicit=None):
        self.name, self.variants = name, variants   # variants: [(name, [field types])]
        self.explicit = explicit or [None] * len(variants)   # `= N` written on a variant without values

    def size(self):
        total = 0
        for _, fts in self.variants:
            n = 1
            for f in fts:
                n *= size_of(f)
            total += n
        return total

    def decl(self):
        parts = []
        for (vn, fts), d in zip(self.variants, self.explicit):
            parts.append(vn + ("(" + ", ".join(tname(f) for f in fts) + ")" if fts else "") + (" = %d" % d if d is not None else ""))
        return "copy enum %s { %s }" % (self.name, ", ".join(parts))


def tname(t):
    return t if isinstance(t, str) else t.name

def size_of(t):
    return 2 if t == "bool" else 5 if t == "i32" else t.size()

def domain(t):
    if t == "bool":
        return [False, True]
    if t == "i32":
        return list(INT_VALUES)
    out = []
    for vi, (_, fts) in enumerate(t.variants):
        for combo in itertools.product(*[domain(f) for f in fts]):
            out.append((vi, list(combo)))
    return out

def render_value(t, v):
    if t == "bool":
        return "true" if v else "false"
    if t == "i32":
        return str(v)
    vi, fields = v
    vn, fts = t.variants[vi]
    if not fts:
        return "%s::%s" % (t.name, vn)
    return "%s::%s(%s)" % (t.name, vn, ", ".join(render_value(ft, fv) for ft, fv in zip(fts, fields)))


def make_enums(rng):
    enums = []
    for i in range(rng.randint(1, 3)):
        variants = []
        for j in range(rng.randint(1, 4)):
            fts = [rng.choice(["bool", "i32"] + enums) for _ in range(rng.choice([0, 0, 1, 1, 2, 3]))]
            variants.append(("V%d" % j, fts))
        explicit = [None] * len(variants)
        if rng.random() < 0.4:      # some plain variants get a number of their own; none may repeat another's
            for j, (_, fts) in enumerate(variants):
                if not fts and rng.random() < 0.6:
                    explicit[j] = rng.choice(DISCRIMINANTS)
            seen, nxt = set(), 0
            for d in explicit:
                eff = nxt if d is None else d
                nxt = eff + 1
                if eff in seen:
                    explicit = [None] * len(variants)
                    break
                seen.add(eff)
        enums.append(Enum("E%d" % i, variants, explicit))
    return enums


class Names:
    def __init__(self):
        self.n = 0
        self.types = {}
    def new(self, t):
        self.n += 1
        name = "b%d" % self.n
        self.types[name] = t
        return name


def gen_pat(rng, t, depth, may_bind, names):
    r = rng.random()
    if r < 0.12:
        return ("_",)
    if r < 0.27 and may_bind:
        return ("bind", names.new(t))
    if t == "bool":
        return ("lit", rng.random() < 0.5)
    if t == "i32":
        if rng.random() < 0.25:
            return ("or", [("lit", rng.choice(INT_LITERALS)) for _ in range(rng.randint(2, 3))])
        return ("lit", rng.choice(INT_LITERALS))
    if depth <= 0:
        return ("_",)
    if rng.random() < 0.2:      # alternatives never bind names
        return ("or", [gen_pat(rng, t, depth - 1, False, names) for _ in range(rng.randint(2, 3))])
    vi = rng.randrange(len(t.variants))
    return ("var", t, vi, [gen_pat(rng, ft, depth - 1, may_bind, names) for ft in t.variants[vi][1]])


def wild_or_bind(rng, t, names):
    return ("_",) if rng.random() < 0.5 else ("bind", names.new(t))


def variant_arms(rng, t, vi, depth, names):
    """arms for variant vi that together match every value it can carry; one or two of its values are split up"""
    fts = t.variants[vi][1]
    if not fts:
        return [("var", t, vi, [])]
    split = rng.sample(range(len(fts)), min(len(fts), rng.choice([1, 1, 2])))
    lists = [cover(rng, fts[k], depth, names) for k in split]
    arms = []
    for combo in itertools.product(*lists):
        subs = [wild_or_bind(rng, ft, names) for ft in fts]
        for k, sp in zip(split, combo):
            subs[k] = sp
        arms.append(("var", t, vi, subs))
    return arms


def cover(rng, t, depth, names):
    """patterns that together match every value of type t, in an order that keeps each one reachable"""
    if depth <= 0 or rng.random() < 0.35:
        return [wild_or_bind(rng, t, names)]
    if t == "bool":
        both = [("lit", True), ("lit", False)]
        rng.shuffle(both)
        return both
    if t == "i32":
        return [("lit", v) for v in rng.sample(INT_LITERALS, rng.randint(0, 3))] + [wild_or_bind(rng, t, names)]
    return [a for vi in range(len(t.variants)) for a in variant_arms(rng, t, vi, depth - 1, names)]


def gen_arms(rng, st, names):
    if rng.random() < 0.3:      # anything goes
        return [gen_pat(rng, st, 3, True, names) for _ in range(rng.randint(1, 7))]
    if isinstance(st, Enum):    # an arm group per variant, in random order
        groups = [variant_arms(rng, st, vi, 2, names) for vi in range(len(st.variants))]
        rng.shuffle(groups)
        arms = [a for g in groups for a in g]
    else:
        arms = cover(rng, st, 2, names)
    r = rng.random()
    if r < 0.2 and len(arms) > 1:        # probably no longer exhaustive
        del arms[rng.randrange(len(arms))]
    elif r < 0.4:                        # probably a dead arm
        arms.insert(rng.randrange(len(arms) + 1), gen_pat(rng, st, 2, True, names))
    for i in range(len(arms) - 1):       # two neighbours that bind nothing can be joined with '|'
        if rng.random() < 0.15 and not binds_of(arms[i], []) and not binds_of(arms[i + 1], []):
            arms[i:i + 2] = [("or", [arms[i], arms[i + 1]])]
            break
    return arms


def render_pat(p):
    k = p[0]
    if k == "_":
        return "_"
    if k == "bind":
        return p[1]
    if k == "lit":
        v = p[1]
        return ("true" if v else "false") if isinstance(v, bool) else str(v)
    if k == "or":
        return " | ".join(render_pat(a) for a in p[1])
    _, t, vi, subs = p
    vn = t.variants[vi][0]
    return "%s::%s" % (t.name, vn) + ("(" + ", ".join(render_pat(s) for s in subs) + ")" if subs else "")


def matches(p, v, env):
    k = p[0]
    if k == "_":
        return True
    if k == "bind":
        env[p[1]] = v
        return True
    if k == "lit":
        return v == p[1] and type(v) == type(p[1])
    if k == "or":
        return any(matches(a, v, {}) for a in p[1])
    _, t, vi, subs = p
    return v[0] == vi and all(matches(s, fv, env) for s, fv in zip(subs, v[1]))


def contribution(t, v):       # what a bound name adds to the result (see the helper functions in the program)
    if t == "bool":
        return 1 if v else 2
    if t == "i32":
        return v
    return (v[0] + 1) * 10


def contribution_expr(t, name):
    if t == "bool":
        return "b2i(%s)" % name
    if t == "i32":
        return name
    return "num_%s(%s)" % (t.name, name)


def binds_of(p, out):
    if p[0] == "bind":
        out.append(p[1])
    elif p[0] == "var":
        for s in p[3]:
            binds_of(s, out)
    return out


def helpers(enums):
    lines = ["fn b2i(b: bool) -> i32 {", "    return match b { true => 1, false => 2 };", "}"]
    for e in enums:
        lines.append("fn num_%s(x: %s) -> i32 {" % (e.name, e.name))
        lines.append("    return match x {")
        for vi, (vn, fts) in enumerate(e.variants):
            lines.append("        %s::%s%s => %d," % (e.name, vn, "(" + ", ".join("_" for _ in fts) + ")" if fts else "", (vi + 1) * 10))
        lines.append("    };")
        lines.append("}")
    return lines


def program(enums, st, arms, names, values, as_statement):
    out = [e.decl() for e in enums] + helpers(enums)
    out.append("fn f(x: %s) -> i32 {" % tname(st))
    body = []
    for i, p in enumerate(arms):
        expr = " + ".join([str(i * 1000)] + [contribution_expr(names.types[b], b) for b in binds_of(p, [])])
        if as_statement:
            body.append("        %s => { return %s; }" % (render_pat(p), expr))
        else:
            body.append("        %s => %s," % (render_pat(p), expr))
    if as_statement:
        out += ["    match x {"] + body + ["    }", "}"]
    else:
        out += ["    return match x {"] + body + ["    };", "}"]
    out.append("fn main() -> i32 {")
    for v in values:
        out.append("    print(f(%s));" % render_value(st, v))
    out += ["    return 0;", "}"]
    return "\n".join(out) + "\n"


def expected(arms, names, v):
    for i, p in enumerate(arms):
        env = {}
        if matches(p, v, env):
            return i * 1000 + sum(contribution(names.types[b], env[b]) for b in binds_of(p, []))
    return None


def brute_verdict(st, arms):
    dom = domain(st)
    taken = [False] * len(arms)
    for v in dom:
        first = None
        for i, p in enumerate(arms):
            if matches(p, v, {}):
                first = i
                break
        if first is None:
            return "noexh"
        taken[first] = True
    # an arm is reachable if it is the first match of some value; an arm that only ever loses to an earlier one is dead
    for i, p in enumerate(arms):
        if not taken[i]:
            return "dead"
    return "ok"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def one_case(n):
    rng = random.Random(SEED * 1000003 + n)
    for _ in range(100):
        enums = make_enums(rng)
        pool = ["bool", "i32"] + enums
        st = rng.choice(enums + enums + pool[:2])
        if size_of(st) <= 1500:
            break
    names = Names()
    arms = gen_arms(rng, st, names)
    want = brute_verdict(st, arms)
    dom = domain(st)
    sample = dom if len(dom) <= 40 else rng.sample(dom, 40)
    src = program(enums, st, arms, names, sample, rng.random() < 0.5)
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "p.nge")
        open(path, "w").write(src)
        r = run([NOGEC] + NOGEC_FLAGS + ["-o", os.path.join(d, "p.c"), path])
        if r.returncode == 0:
            got = "ok"
        elif "match is not exhaustive" in r.stderr:
            got = "noexh"
        elif "this arm can never match" in r.stderr:
            got = "dead"
        else:
            return ("unexpected compiler error", src, r.stderr, want, None)
        if got != want:
            return ("verdict differs", src, "compiler says %s, brute force says %s" % (got, want), want, got)
        if got != "ok":
            return (None, None, None, want, got)
        c = run(CC + CFLAGS_WARN + ["-o", os.path.join(d, "p"), os.path.join(d, "p.c")])
        if c.returncode != 0:
            return ("generated C does not build", src, c.stderr, want, got)
        e = run([os.path.join(d, "p")])
        if e.returncode != 0:
            return ("program failed", src, e.stderr, want, got)
        results = [expected(arms, names, v) for v in sample]
        if e.stdout.split() != [str(x) for x in results]:
            return ("results differ", src, "got      %s\nexpected %s" % (e.stdout.split(), results), want, got)
    return (None, None, None, want, got)


def main():
    counts = {"ok": 0, "noexh": 0, "dead": 0}
    failures = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for n, (problem, src, detail, want, got) in zip(range(CASES), pool.map(lambda n: one_case(n), range(CASES))):
            counts[want] += 1
            if problem:
                failures += 1
                if failures <= 5:
                    print("FAIL case %d: %s\n%s\n%s" % (n, problem, detail, src))
    print("%d cases (%d accepted and run, %d not exhaustive, %d with a dead arm): %s" % (
        CASES, counts["ok"], counts["noexh"], counts["dead"], "all agree" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
