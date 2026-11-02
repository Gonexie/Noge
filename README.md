# Noge

A small systems language for embedded targets: memory safety first, fast compiles second.
The compiler (`nogec`) is written in C99 and emits C. See [docs/design.md](docs/design.md).

```
make              # builds build/nogec
make test         # tests/ok, tests/trap and tests/err (about 250 checks, run twice where it matters)
make check-arith  # slower: checks the overflow/division/shift code against __int128 arithmetic

build/nogec -o out.c prog.nge && cc -o out out.c && ./out
```

## The language so far

```
copy struct Pt { x: i32, y: i32 }       // `copy`: using it by value duplicates it
struct Buf { data: [u8; 64], len: u32 } // otherwise it is move-only

fn push(b: inout Buf, v: u8) -> bool {  // the callee may change the caller's value
    if b.len >= len(b.data) { return false; }
    b.data[b.len] = v;                  // bounds-checked
    b.len = b.len + 1;                  // overflow-checked
    return true;
}

fn main() -> i32 {
    var b = Buf { data: [0; 64], len: 0 };
    push(&b, 7);                        // `&` marks inout / set arguments
    print(b.data[0] as i32);
    return 0;
}
```

Types: `i8..i64`, `u8..u64`, `bool`, `struct` (by value, nested), fixed arrays `[T; N]` (nested, inside structs,
of structs). Statements: `let`, `var`, `if` / `else if` / `else`, `while`, `return`, assignment.
Expressions: arithmetic `+ - * / %`, bitwise `& | ^ ~ << >>`, comparison, `&& || !`, `as` casts between integer
types (and from `bool`), struct literals, array literals `[a, b, c]` and `[value; N]`, field access, indexing.
Precedence follows Rust (`& | ^` bind tighter than `==`). Builtins: `print(i32)`, `len(array)` (a compile-time
constant that takes its integer type from context), `wrapping_add` / `wrapping_sub` / `wrapping_mul`.

Parameter conventions:

| mode    | caller writes | callee may                         | caller afterwards                       |
|---------|---------------|------------------------------------|-----------------------------------------|
| `let`   | `x`           | read it (default)                  | unchanged                               |
| `inout` | `&x`          | read and change it                 | sees the change                         |
| `set`   | `&x`          | must fully initialize it           | `x` is initialized                      |
| `sink`  | `x`           | keep it, consume it (immutable)    | `x` is moved, unless its type is `copy` |

`let` parameters of struct or array type are passed by pointer in the emitted C, so large values are not copied.

## What is checked at compile time

The type checker (`src/check.c`) handles types and mutability. The flow pass (`src/flow.c`) handles the rest.

- **Definite initialization.** Every scalar field of every variable is tracked as uninitialized, initialized or
  moved (an array counts as one unit). A place must be fully initialized to be read, passed or moved. Branches and
  loops are joined conservatively, so `var x: i32; if c { x = 1; } print(x);` is rejected.
- **Moves.** Using a non-`copy` place by value (`let b = a;`, `return a;`, a `sink` argument, a literal field)
  moves it, and it cannot be used again until it is assigned. Partial moves work: moving `p.a` leaves `p.b` usable.
  Loops are analysed until their state stops changing, so a move in one iteration followed by a use in the next is
  caught. Nothing can be moved out of a `let` parameter, or out of one element of an array.
- **Parameters.** `inout` and `set` parameters must be whole whenever the function returns.
- **Exclusivity.** Arguments are evaluated left to right. A `let`, `inout` or `set` argument that names a place
  borrows it until the call returns. Any later access that overlaps a live borrow, including one inside a later
  argument or a nested call, is an error unless both are plain reads. So `f(&a, &a)`, `f(&a, a)` and
  `f(a, g(&a))` are rejected; `f(&a.x, &a.y)`, `f(a, a)` and `f(g(&a), &a)` are fine, and a `sink` argument is
  copied or moved when it is evaluated: `f(a, &a)` is fine for a `sink` parameter and an error for a `let` one.
  Array elements are chosen at run time, so borrowing two elements of one array at once is rejected.
- **Literals.** A literal index outside its array, division or remainder by literal zero, and a literal shift
  amount outside `0 .. bits-1` are compile errors.

Errors name the offending place, for example `use of moved value 'p.a.len'`, and stop at the first one.

## What is checked at run time

The emitted C does not rely on undefined behaviour for arithmetic, shifts or indexing. Stack depth is not checked yet. These checks end in `NOGE_TRAP(kind, line)`:

| check                                                     | trap                          |
|-----------------------------------------------------------|-------------------------------|
| `+ - *` and unary `-` that do not fit the type            | integer overflow              |
| `/` or `%` by zero                                        | division by zero              |
| `MIN / -1` and `MIN % -1` for signed types               | integer overflow              |
| `<<` or `>>` by a count that is negative or `>= bits`     | shift amount out of range     |
| array index that is negative or `>= N`                    | array index out of bounds     |

`wrapping_add/sub/mul` never trap; they give the modular result. `as` truncates or sign-extends and `<<` drops
the bits shifted out, as in Rust. Evaluation order is fixed: operands, arguments, array elements and literal
fields left to right, an assignment's right side before its left place, and `&&` / `||` short-circuit.

The default hook prints `file:line: runtime error: ...` and exits with status 101. To reset the chip or log
instead, define your own when compiling the generated C (it must not return). `kind` is 1 overflow,
2 division by zero, 3 bounds, 4 shift:

```
cc -D'NOGE_TRAP(kind,line)=my_fault((kind),(line))' -include my_fault.h out.c
```

The overflow checks use `__builtin_*_overflow` (GCC, Clang). `nogec --no-builtins` emits plain C99 checks
instead, for other compilers; `make test` runs every program both ways.

## Pipeline

```
src/lexer.c   bytes -> tokens                src/lower.c   AST -> IR: evaluation order, run-time checks
src/parser.c  tokens -> AST                  src/emit_c.c  IR -> C99
src/check.c   names, types, mutability       src/ir.h      the IR (structured three-address code)
src/flow.c    init, moves, exclusivity
```

On a 68,000-line generated program `nogec` takes about 0.25 s; compiling the 9.6 MB of C it produces takes 25 s at
`cc -O0`, so the C compiler, not `nogec`, is where the time goes.

## Known gaps (planned milestones)

- Arena and handle types (`Arena<T, N>`, `Id<T>`) need generics. The pattern already works in plain code
  (see `tests/ok/arena.nge`): a struct holding an array and a length, with `u32` or `i32` indices.
- Constant expressions are not folded, so `255 + 1` on `u8` traps at run time instead of failing to compile,
  and only literal indices are range-checked at compile time.
- The emitted C relies on what GCC and Clang do for two things the C standard leaves implementation-defined:
  converting an out-of-range value to a signed type (`as`, and `<<`) and `>>` of a negative value.
- Dropping a non-`copy` value without using it is allowed (there are no destructors yet).
- `let x: T;` (assign later) is not supported, only `var x: T;`.
- Recursion is not restricted yet, and `print` needs `stdio`.
- The compiler stops at the first error, and the only backend is C.
- Enums and pattern matching, generics, separate compilation.
