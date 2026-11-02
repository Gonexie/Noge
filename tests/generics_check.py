#!/usr/bin/env python3
"""Differential test of generics: a generic program must behave like the same program written out by hand.

Each random case is a program that uses a small library of generic types and functions (a pair, an optional
value, a fixed-size array with a const parameter, a wrapper that uses the optional one, and functions over
them) with random type arguments, in two forms:

  generic   the templates as written, the uses leaving the compiler to work out the arguments (from other
            arguments, from the type a result is wanted as, or from what is written out with ::<...>)
  expanded  the same program with every instance written out as a plain struct, enum and function

Both are compiled (with -Wall -Wextra -Wpedantic -Werror) and run; they must both compile and print the same.
This checks inference, copying of declarations with their parameters replaced, const arguments, the naming of
instances and the copy rules, against hand-written code that uses none of them.

usage: tests/generics_check.py [path/to/nogec] [--cases N] [--seed S]        (also: make check-generics)
"""
import os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

args = list(sys.argv[1:])
def option(name, default):
    if name in args:
        i = args.index(name)
        value = int(args[i + 1])
        del args[i:i + 2]
        return value
    return default
CASES = option("--cases", 800)
SEED = option("--seed", 1)
NOGEC = args[0] if args else "build/nogec"
CC = os.environ.get("CC", "cc").split()
CFLAGS_WARN = os.environ.get("CFLAGS_WARN", "-std=c99 -Wall -Wextra -Wpedantic -Werror").split()
NOGEC_FLAGS = os.environ.get("NOGEC_FLAGS", "").split()

LEAVES = ["i32", "u8", "bool", "i64"]

# ---- types: a leaf name, or ("P", a, b), ("O", t), ("Arr", t, n), ("W", t)
def gname(t):
    if isinstance(t, str): return t
    if t[0] == "P": return "P<%s, %s>" % (gname(t[1]), gname(t[2]))
    if t[0] == "Arr": return "Arr<%s, %d>" % (gname(t[1]), t[2])
    return "%s<%s>" % (t[0], gname(t[1]))

def mname(t):
    if isinstance(t, str): return t
    if t[0] == "P": return "P_%s_%s" % (mname(t[1]), mname(t[2]))
    if t[0] == "Arr": return "Arr_%s_%d" % (mname(t[1]), t[2])
    return "%s_%s" % (t[0], mname(t[1]))

def rand_type(rng, depth):
    if depth <= 0 or rng.random() < 0.45:
        return rng.choice(LEAVES)
    k = rng.choice(["P", "O", "Arr", "W"])
    if k == "P": return ("P", rand_type(rng, depth - 1), rand_type(rng, depth - 1))
    if k == "Arr": return ("Arr", rand_type(rng, depth - 1), rng.randint(1, 4))
    return (k, rand_type(rng, depth - 1))

# ---- the library, written once as generic templates
GENERIC_LIB = """copy struct P<A, B> { a: A, b: B }
copy enum O<T> { S(T), N }
copy struct Arr<T, const N: usize> { items: [T; N], n: i32 }
copy struct W<T> { inner: O<T>, tag: i32 }

fn p_swap<A, B>(p: P<A, B>) -> P<B, A> { return P { a: p.b, b: p.a }; }
fn p_first<A, B>(p: P<A, B>) -> A { return p.a; }
fn o_or<T>(o: O<T>, d: T) -> T {
    return match o {
        O::S(v) => v,
        O::N => d,
    };
}
fn o_is<T>(o: O<T>) -> i32 {
    return match o {
        O::S(_) => 1,
        O::N => 0,
    };
}
fn arr_new<T, const N: usize>(fill: T) -> Arr<T, N> { return Arr { items: [fill; N], n: 0 }; }
fn arr_set<T, const N: usize>(a: inout Arr<T, N>, i: usize, v: T) {
    a.items[i] = v;
    a.n = a.n + 1;
}
fn arr_get<T, const N: usize>(a: Arr<T, N>, i: usize) -> T { return a.items[i]; }
fn arr_len<T, const N: usize>(a: Arr<T, N>) -> usize { return N; }
fn arr_n<T, const N: usize>(a: Arr<T, N>) -> i32 { return a.n; }
fn arr_sum<const N: usize>(a: Arr<i32, N>) -> i32 {
    var i: usize = 0;
    var s = 0;
    while i < N {
        if a.items[i] > 0 {
            s = s + a.items[i];
        } else {
            s = s - (N as i32);
        }
        i = i + 1;
    }
    return s;
}
fn w_make<T>(x: T) -> W<T> { return W { inner: O::S(x), tag: 7 }; }
fn w_get<T>(w: W<T>, d: T) -> T { return o_or(w.inner, d); }
fn w_tag<T>(w: W<T>) -> i32 { return w.tag; }
"""


class Expanded:
    """the same library with every instance a plain declaration, made on demand and kept in dependency order"""
    def __init__(self):
        self.decls, self.seen = [], set()

    def once(self, key, text):
        if key not in self.seen:
            self.seen.add(key)
            self.decls.append(text)

    def type(self, t):
        if isinstance(t, str): return t
        m = mname(t)
        if ("type", m) in self.seen: return m
        if t[0] == "P": text = "copy struct %s { a: %s, b: %s }" % (m, self.type(t[1]), self.type(t[2]))
        elif t[0] == "O": text = "copy enum %s { S(%s), N }" % (m, self.type(t[1]))
        elif t[0] == "Arr": text = "copy struct %s { items: [%s; %d], n: i32 }" % (m, self.type(t[1]), t[2])
        else: text = "copy struct %s { inner: %s, tag: i32 }" % (m, self.type(("O", t[1])))
        self.once(("type", m), text)
        return m

    def fn(self, name, t):
        """the instance of library function `name` for type t (the type of its first argument); returns its name"""
        T = self.type
        if name in ("p_swap", "p_first"):
            a, b = t[1], t[2]
            m = "%s_%s_%s" % (name, mname(a), mname(b))
            if name == "p_swap":
                r = ("P", b, a)
                text = "fn %s(p: %s) -> %s { return %s { a: p.b, b: p.a }; }" % (m, T(t), T(r), T(r))
            else:
                text = "fn %s(p: %s) -> %s { return p.a; }" % (m, T(t), T(a))
        elif name in ("o_or", "o_is"):
            x = t[1]
            m = "%s_%s" % (name, mname(x))
            if name == "o_or":
                text = "fn %s(o: %s, d: %s) -> %s {\n    return match o {\n        %s::S(v) => v,\n        %s::N => d,\n    };\n}" % (
                    m, T(t), T(x), T(x), T(t), T(t))
            else:
                text = "fn %s(o: %s) -> i32 {\n    return match o {\n        %s::S(_) => 1,\n        %s::N => 0,\n    };\n}" % (
                    m, T(t), T(t), T(t))
        elif name.startswith("arr_"):
            x, n = t[1], t[2]
            m = "%s_%s_%d" % (name, mname(x), n)
            A = T(t)
            text = {
                "arr_sum": "fn %s(a: %s) -> i32 {\n    var i: usize = 0;\n    var s = 0;\n    while i < %d {\n        if a.items[i] > 0 {\n            s = s + a.items[i];\n        } else {\n            s = s - (%d as i32);\n        }\n        i = i + 1;\n    }\n    return s;\n}" % (m, A, n, n),
                "arr_new": "fn %s(fill: %s) -> %s { return %s { items: [fill; %d], n: 0 }; }" % (m, T(x), A, A, n),
                "arr_set": "fn %s(a: inout %s, i: usize, v: %s) {\n    a.items[i] = v;\n    a.n = a.n + 1;\n}" % (m, A, T(x)),
                "arr_get": "fn %s(a: %s, i: usize) -> %s { return a.items[i]; }" % (m, A, T(x)),
                "arr_len": "fn %s(a: %s) -> usize { return %d; }" % (m, A, n),
                "arr_n": "fn %s(a: %s) -> i32 { return a.n; }" % (m, A),
            }[name]
        else:  # w_make (t is the type of the value wrapped), w_get and w_tag (t is the wrapper)
            x = t if name == "w_make" else t[1]
            m = "%s_%s" % (name, mname(x))
            W, O = T(("W", x)), T(("O", x))
            if name == "w_make":
                text = "fn %s(x: %s) -> %s { return %s { inner: %s::S(x), tag: 7 }; }" % (m, T(x), W, W, O)
            elif name == "w_get":
                text = "fn %s(w: %s, d: %s) -> %s { return %s(w.inner, d); }" % (m, W, T(x), T(x), self.fn("o_or", ("O", x)))
            else:
                text = "fn %s(w: %s) -> i32 { return w.tag; }" % (m, W)
        self.once(("fn", m), text)
        return m


# ---- values: data first, so that each form can spell the same value its own way
def rand_value(rng, t):
    if isinstance(t, str):
        text = {"bool": lambda: rng.choice(["true", "false"]), "u8": lambda: str(rng.randint(0, 200)),
                "i64": lambda: str(rng.randint(-1000, 1000)), "i32": lambda: str(rng.randint(-50, 50))}[t]()
        return ("leaf", t, text)
    if t[0] == "P": return ("P", t, rand_value(rng, t[1]), rand_value(rng, t[2]))
    if t[0] == "O": return ("O", t, None if rng.random() < 0.3 else rand_value(rng, t[1]))
    if t[0] == "Arr": return ("Arr", t, [rand_value(rng, t[1]) for _ in range(t[2])], rng.randint(0, 5))
    return ("W", t, rand_value(rng, t[1]), rng.randint(0, 9))

def render(v, mono, exp):
    k, t = v[0], v[1]
    if k == "leaf": return v[2]
    if k == "P":
        body = "{ a: %s, b: %s }" % (render(v[2], mono, exp), render(v[3], mono, exp))
        return (exp.type(t) + " " + body) if mono else "P " + body
    if k == "O":
        if v[2] is None: return ("%s::N" % exp.type(t)) if mono else "O::N"
        inner = render(v[2], mono, exp)
        return ("%s::S(%s)" % (exp.type(t), inner)) if mono else "O::S(%s)" % inner
    if k == "Arr":
        body = "{ items: [%s], n: %d }" % (", ".join(render(x, mono, exp) for x in v[2]), v[3])
        return (exp.type(t) + " " + body) if mono else "Arr " + body
    inner = ("%s::S(%s)" % (exp.type(("O", t[1])), render(v[2], mono, exp))) if mono else "O::S(%s)" % render(v[2], mono, exp)
    body = "{ inner: %s, tag: %d }" % (inner, v[3])
    return (exp.type(t) + " " + body) if mono else "W " + body


def build(seed):
    rng = random.Random(seed)
    exp = Expanded()
    gen, mono, env, counter = [], [], [], [0]

    def fresh():
        counter[0] += 1
        return "v%d" % counter[0]

    def emit(g, m):
        gen.append(g)
        mono.append(m)

    def show(t, eg, em):
        """print every leaf of the value expressions eg (generic form) and em (expanded form) of type t"""
        if isinstance(t, str):
            emit("print(%s as i32);" % eg, "print(%s as i32);" % em)
        elif t[0] == "P":
            show(t[1], "p_first(%s)" % eg, "%s(%s)" % (exp.fn("p_first", t), em))
            show(t[2], "%s.b" % eg, "%s.b" % em)
        elif t[0] == "O":
            emit("print(o_is(%s));" % eg, "print(%s(%s));" % (exp.fn("o_is", t), em))
            d = rand_value(rng, t[1])
            show(t[1], "o_or(%s, %s)" % (eg, render(d, False, exp)), "%s(%s, %s)" % (exp.fn("o_or", t), em, render(d, True, exp)))
        elif t[0] == "Arr":
            emit("print(arr_len(%s) as i32);" % eg, "print(%s(%s) as i32);" % (exp.fn("arr_len", t), em))
            emit("print(arr_n(%s));" % eg, "print(%s(%s));" % (exp.fn("arr_n", t), em))
            if t[1] == "i32":
                emit("print(arr_sum(%s));" % eg, "print(%s(%s));" % (exp.fn("arr_sum", t), em))
            show(t[1], "arr_get(%s, 0)" % eg, "%s(%s, 0)" % (exp.fn("arr_get", t), em))
        else:
            emit("print(w_tag(%s));" % eg, "print(%s(%s));" % (exp.fn("w_tag", t), em))
            d = rand_value(rng, t[1])
            show(t[1], "w_get(%s, %s)" % (eg, render(d, False, exp)), "%s(%s, %s)" % (exp.fn("w_get", t), em, render(d, True, exp)))

    for _ in range(rng.randint(3, 8)):
        pairs = [(n, t) for n, t in env if not isinstance(t, str) and t[0] == "P"]
        action = rng.choice(["new", "new", "swap", "wrap", "opt", "arr", "arr"])
        if action == "swap" and pairs:
            n, t = rng.choice(pairs)
            v, r = fresh(), ("P", t[2], t[1])
            emit("let %s = p_swap(%s);" % (v, n), "let %s = %s(%s);" % (v, exp.fn("p_swap", t), n))
        elif action == "wrap" and env:
            n, t = rng.choice(env)
            v, r = fresh(), ("W", t)
            emit("let %s = w_make(%s);" % (v, n), "let %s = %s(%s);" % (v, exp.fn("w_make", t), n))
        elif action == "opt" and env:
            n, t = rng.choice(env)
            v, r = fresh(), ("O", t)
            emit("let %s = O::S(%s);" % (v, n), "let %s = %s::S(%s);" % (v, exp.type(r), n))
        elif action == "arr":
            x, size = rand_type(rng, 1), rng.randint(1, 4)
            r = ("Arr", x, size)
            v = fresh()
            fill = rand_value(rng, x)
            if rng.random() < 0.5:   # the type the result is wanted as decides the arguments
                emit("var %s: %s = arr_new(%s);" % (v, gname(r), render(fill, False, exp)),
                     "var %s: %s = %s(%s);" % (v, exp.type(r), exp.fn("arr_new", r), render(fill, True, exp)))
            else:                    # or they are written out
                emit("var %s = arr_new::<%s, %d>(%s);" % (v, gname(x), size, render(fill, False, exp)),
                     "var %s = %s(%s);" % (v, exp.fn("arr_new", r), render(fill, True, exp)))
            for _ in range(rng.randint(0, 3)):
                val, idx = rand_value(rng, x), rng.randrange(size)
                emit("arr_set(&%s, %d, %s);" % (v, idx, render(val, False, exp)),
                     "%s(&%s, %d, %s);" % (exp.fn("arr_set", r), v, idx, render(val, True, exp)))
        else:
            r = rand_type(rng, 2)
            v = fresh()
            val = rand_value(rng, r)
            emit("let %s: %s = %s;" % (v, gname(r), render(val, False, exp)),
                 "let %s: %s = %s;" % (v, exp.type(r), render(val, True, exp)))
        env.append((v, r))
        show(r, v, v)

    generic = GENERIC_LIB + "\nfn main() -> i32 {\n" + "".join("    %s\n" % s for s in gen) + "    return 0;\n}\n"
    expanded = "\n".join(exp.decls) + "\n\nfn main() -> i32 {\n" + "".join("    %s\n" % s for s in mono) + "    return 0;\n}\n"
    return generic, expanded


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build_and_run(d, name, src):
    path = os.path.join(d, name + ".nge")
    open(path, "w").write(src)
    r = run([NOGEC] + NOGEC_FLAGS + ["-o", os.path.join(d, name + ".c"), path])
    if r.returncode != 0:
        return None, "nogec rejected it:\n" + r.stderr
    c = run(CC + CFLAGS_WARN + ["-o", os.path.join(d, name), os.path.join(d, name + ".c")])
    if c.returncode != 0:
        return None, "the C does not build:\n" + c.stderr
    e = run([os.path.join(d, name)])
    if e.returncode != 0:
        return None, "the program failed:\n" + e.stderr
    return e.stdout, None


def one_case(n):
    generic, expanded = build(SEED * 1000003 + n)
    with tempfile.TemporaryDirectory() as d:
        out_g, err_g = build_and_run(d, "generic", generic)
        out_e, err_e = build_and_run(d, "expanded", expanded)
        if err_e:
            return "the expanded form: " + err_e, generic, expanded
        if err_g:
            return "the generic form: " + err_g, generic, expanded
        if out_g != out_e:
            return "different output:\n  generic  %s\n  expanded %s" % (out_g.split(), out_e.split()), generic, expanded
    return None


def main():
    failures = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        for n, res in zip(range(CASES), pool.map(one_case, range(CASES))):
            if res:
                failures += 1
                if failures <= 3:
                    print("FAIL case %d: %s\n--- generic ---\n%s\n--- expanded ---\n%s" % (n, res[0], res[1], res[2]))
    print("%d programs: %s" % (CASES, "generic and hand-expanded forms agree" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
