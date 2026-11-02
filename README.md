# Noge

A small systems language for embedded targets: memory safety first, fast compiles second.
The compiler (`nogec`) is written in C99 and emits C. See [docs/design.md](docs/design.md).

```
make          # builds build/nogec
make test     # tests/ok: compile, build, run, compare.  tests/err: must be rejected with a given message

build/nogec -o out.c prog.nge && cc -o out out.c && ./out
```

## The language so far

```
copy struct Pt { x: i32, y: i32 }       // `copy`: using it by value duplicates it
struct Buf { len: u32, cap: u32 }        // otherwise it is move-only

fn bump(p: inout Pt) { p.x = p.x + 1; }  // the callee may change the caller's value
fn init(b: set Buf) { b.len = 0; b.cap = 8; }   // the callee must initialize it
fn eat(b: sink Buf) { }                  // the callee takes ownership

fn main() -> i32 {
    var p = Pt { x: 1, y: 2 };
    bump(&p);                            // `&` marks inout / set arguments
    var b: Buf;                          // declared now, assigned later
    init(&b);
    eat(b);                              // b is moved: using it again is an error
    print(p.x);
    return 0;
}
```

Types `i8..i64`, `u8..u64`, `bool`, `struct` (by value, nested). Statements `let`, `var`, `if` / `else if` / `else`,
`while`, `return`, assignment. Expressions: arithmetic, comparison, `&&` `||` `!`, `as` casts between integer types
(and from `bool`), struct literals, field access. Builtin `print(i32)`.

Parameter conventions:

| mode    | caller writes | callee may                         | caller afterwards                 |
|---------|---------------|------------------------------------|-----------------------------------|
| `let`   | `x`           | read it (default)                  | unchanged                         |
| `inout` | `&x`          | read and change it                 | sees the change                   |
| `set`   | `&x`          | must fully initialize it           | `x` is initialized                |
| `sink`  | `x`           | keep it, consume it (immutable)    | `x` is moved, unless its type is `copy` |

## Safety rules (checked at compile time)

The type checker (`src/check.c`) handles types and mutability. The flow pass (`src/flow.c`) handles the rest.

- **Definite initialization.** Every scalar field of every variable is tracked as uninitialized, initialized or moved.
  A place must be fully initialized to be read, passed or moved. Branches and loops are joined conservatively, so
  `var x: i32; if c { x = 1; } print(x);` is rejected. Fields can be initialized one at a time.
- **Moves.** Using a non-`copy` place by value (`let b = a;`, `return a;`, a `sink` argument, a struct-literal
  field) moves it, and a moved place cannot be used until it is assigned again. Partial moves work:
  moving `p.a` leaves `p.b` usable. A loop body is analysed until its state stops changing, so moving a value in
  one iteration and using it in the next is caught.
- **Nothing can be moved out of a `let` parameter**, because the caller still owns it.
- **`inout` and `set` parameters must be whole when the function returns** (on every return and at the end).
  You can take a field out of an `inout` parameter, but you have to put a replacement back first.
- **Exclusivity.** Within one expression (call arguments, binary operands, struct-literal fields), accesses to
  overlapping storage must all be plain reads. `f(&a, &a)`, `f(&a, a)`, `f(&a.x, &a)` and `a.x + g(&a)` are rejected;
  `f(&a.x, &a.y)` and `f(a, a)` are fine. This is deliberately stricter than needed, because C does not fix
  the order in which operands and arguments are evaluated.

Errors name the offending place, for example `use of moved value 'p.a.len'`, and stop at the first one.

## Known gaps (planned milestones)

- Integer overflow, division by zero and out-of-range shifts are C undefined behaviour for now, and there are no
  arrays or bounds checks. (milestone 3)
- Evaluation order follows C where the exclusivity rule allows it to differ; lowering to an IR with explicit
  temporaries will fix the order and let the exclusivity rule relax to "left to right". (milestone 3)
- Dropping a non-`copy` value without using it is allowed (there are no destructors yet).
- `let x: T;` (assign later) is not supported, only `var x: T;`.
- The compiler stops at the first error.
- The AST uses arena-allocated pointers rather than the flat index arrays described in the design notes; this can
  change without affecting the language.
