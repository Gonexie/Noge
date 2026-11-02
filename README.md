# Noge

A small systems language for embedded targets: memory safety first, fast compiles second.
The compiler (`nogec`) is written in C99 and emits C. See [docs/design.md](docs/design.md).

```
make              # builds build/nogec
make test         # host tests, then the Cortex-M tests under QEMU if the ARM toolchain is installed
make check-arith  # slower: the overflow/division/shift code and the constant folder, against __int128
make -C examples/cortex-m test   # just the firmware (see examples/cortex-m/README.md)

build/nogec -o out.c prog.nge && cc -o out out.c && ./out
build/nogec --target=cortex-m -o fw.c fw.nge      # for a microcontroller: no C library, 32-bit pointers
```

## The language so far

```
copy struct Pt { x: i32, y: i32 }       // `copy`: using it by value duplicates it
struct Buf { data: [u8; 64], len: u32 } // otherwise it is move-only

const CAPACITY: usize = 64;             // known at compile time; usable in array lengths

fn push(b: inout Buf, v: u8) -> bool {  // the callee may change the caller's value
    if b.len >= CAPACITY as u32 { return false; }
    b.data[b.len] = v;                  // bounds-checked
    b.len = b.len + 1;                  // overflow-checked
    return true;
}

fn main() -> i32 {
    var b = Buf { data: [0; CAPACITY], len: 0 };
    push(&b, 7);                        // `&` marks inout / set arguments
    print(b.data[0] as i32);
    return 0;
}
```

Items: `fn`, `struct`, `copy struct`, `const`, `extern "C" fn`, `export "C" fn`.
Types: `i8..i64`, `u8..u64`, `usize`, `isize`, `bool`, `struct` (by value, nested), fixed arrays `[T; N]`, raw
pointers `*mut T` and `*const T`. Statements: `let`, `var`, `if` / `else if` / `else`, `while`, `return`, assignment,
`unsafe { }`. Expressions: arithmetic `+ - * / %`, bitwise `& | ^ ~ << >>`, comparison, `&& || !`, `as` casts (between
integers, from `bool`, between integers and pointers, between pointers), struct literals, array literals `[a, b, c]`
and `[value; N]`, field access, indexing, C strings `c"text"`. Numbers may be written `255`, `0xFF`, `0b1111` and
with separators, `0x4002_1018`. Precedence follows Rust (`& | ^` bind tighter than `==`).

`inout`, `sink`, `set` and `copy` are keywords only where they can be (in front of a parameter's type, in front of
`struct`); elsewhere they are ordinary names.

Parameter conventions:

| mode    | caller writes | callee may                         | caller afterwards                       |
|---------|---------------|------------------------------------|-----------------------------------------|
| `let`   | `x`           | read it (default)                  | unchanged                               |
| `inout` | `&x`          | read and change it                 | sees the change                         |
| `set`   | `&x`          | must fully initialize it           | `x` is initialized                      |
| `sink`  | `x`           | keep it, consume it (immutable)    | `x` is moved, unless its type is `copy` |

`let` parameters of struct or array type are passed by pointer in the emitted C, so large values are not copied.

Builtins: `len(array)` (a constant that takes its integer type from context), `wrapping_add` / `wrapping_sub` /
`wrapping_mul`, `print(i32)` (hosted builds only), and the pointer operations below.

## Constants

`const NAME: T = expr;` declares an integer or bool known at compile time. Constants can use each other in any
order and can be array lengths, so `[u8; BUF_LEN * 2]` works. More generally, **any expression made only of literals
and constants is computed by the compiler**, with the same rules as at run time: `255 + 1` as a `u8`, `5 / (1 - 1)`,
`1 << 40` as an `i32` or an array index that is certainly out of range are compile errors.

## Unsafe code and C

Most programs never need this. Where they do, it is confined to `unsafe { }` blocks, so a reader can find it.

**Raw pointers.** `*mut T` and `*const T` are ordinary values (they can be fields, parameters, results; compared with
`==`; cast with `as` to and from integers and other pointer types: `0x4002_1018 as *mut u32`). *Using* one is unsafe.
These operations need an `unsafe` block:

| operation              | meaning                                                             |
|------------------------|---------------------------------------------------------------------|
| `ptr_read(p)`          | the value `p` points at (the type must be `copy`)                   |
| `ptr_write(p, v)`      | store `v` (`p` must be `*mut`)                                      |
| `volatile_read(p)`     | like `ptr_read`, but never reordered or removed: for device registers |
| `volatile_write(p, v)` | likewise; integers, `bool` and pointers only                        |
| `ptr_offset(p, n)`     | the pointer `n` elements further on                                 |

There is no address-of operator, so a pointer to a Noge variable cannot be made; pointers come from addresses, from C,
or from `c"..."`. That keeps the borrow rules intact: to give C access to a variable, pass it as an `inout`, `set`
or `let` argument of an `extern` function.

**Calling C.** `extern "C" fn name(params) -> T;` declares a function defined in C. Calling it needs `unsafe`.
Parameters keep their modes: scalars and pointers are passed by value; `let` / `inout` / `set` structs and scalars
by pointer; an array by pointer to its first element; `sink` structs by value. A `struct` must have the same fields,
in the same order, as the C struct. Arrays cannot be passed by value or returned.

```
extern "C" fn strlen(s: *const u8) -> usize;
extern "C" fn memset(dst: inout [u8; 8], c: i32, n: usize) -> *mut u8;
unsafe { let n = strlen(c"hello"); }
```

**Being called from C.** `export "C" fn name(params) { ... }` gives the function the plain C name `name`. This is how
interrupt handlers are written: `export "C" fn SysTick_Handler() { ... }`. Noge code can still call it normally.

**Strings.** `c"text"` is a `*const u8` to static, NUL-terminated bytes (escapes `\n \r \t \0 \\ \" \xHH`).

The extern and exported names are attached to the emitted C with assembler labels, so they never clash with the C
library's declarations or with the compiler's own names.

## Targets

By default the output is a hosted program: it includes `<stdio.h>`, `print` works, and a failed run-time check
prints `file:line: runtime error: ...` and exits with status 101.

| option                 | meaning                                                                         |
|------------------------|---------------------------------------------------------------------------------|
| `--freestanding`       | no C library: only `<stdbool.h>` and `<stdint.h>`; `print` is an error          |
| `--ptr-bits=32`        | `usize`, `isize` and pointers are 32 bits wide (default: the host's width, 64)  |
| `--target=cortex-m`    | both of the above                                                               |
| `--no-builtins`        | portable C99 overflow checks instead of `__builtin_*_overflow`                   |

`usize` is as wide as the target's pointers, so `--ptr-bits` must match the C compiler you build the output with.
Only 32- and 64-bit pointers are supported.

The emitted C is warning-free under `-Wall -Wextra -Wpedantic` with GCC 11, 12 and 13 and with clang 18; the test
suite builds it with clang as well when clang is installed. The compiler itself needs GCC or clang (it uses
`__builtin_*_overflow`).

In a freestanding build a failed check calls `noge_trap_hook(kind, line)` (1 overflow, 2 division by zero,
3 index out of bounds, 4 shift), which your board code may define; the weak default loops forever. In a hosted build
define `NOGE_TRAP(kind, line)` instead, for example
`cc -D'NOGE_TRAP(kind,line)=my_fault((kind),(line))' -include my_fault.h out.c`. The hook must not return.

[examples/cortex-m](examples/cortex-m/README.md) has firmware for an ARM Cortex-M3 that runs under QEMU: a blinky
that writes the STM32 GPIO registers (260 bytes of flash, no RAM), an interrupt handler in Noge, CRC-32 and 64-bit
arithmetic, and a failed bounds check reported with its source line.

## What is checked at compile time

The type checker (`src/check.c`) handles types, constants and mutability. The flow pass (`src/flow.c`) handles the rest.

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
- **Unsafe.** Raw-pointer operations and calls to `extern` functions are only allowed inside `unsafe { }`.
- **Constants and literals.** See above.

Errors name the offending place, for example `use of moved value 'p.a.len'`, and stop at the first one.

## What is checked at run time

The emitted C does not rely on undefined behaviour for arithmetic, shifts or indexing. Stack depth is not checked
yet. These checks end in `NOGE_TRAP(kind, line)`:

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

## Pipeline

```
src/lexer.c   bytes -> tokens                src/lower.c   AST -> IR: evaluation order, run-time checks
src/parser.c  tokens -> AST                  src/emit_c.c  IR -> C99
src/check.c   names, types, constants        src/ir.h      the IR (structured three-address code)
src/flow.c    init, moves, exclusivity
```

On a 68,000-line generated program `nogec` takes about 0.25 s; compiling the 9.6 MB of C it produces takes 25 s at
`cc -O0`, so the C compiler, not `nogec`, is where the time goes.

## Known gaps (planned milestones)

- There are no global variables. Shared state between an interrupt handler and `main` lives in memory owned by C
  and is reached through a volatile pointer (see `examples/cortex-m/systick.nge`). Deciding how interrupt-shared
  state should be typed is still open.
- Arena and handle types (`Arena<T, N>`, `Id<T>`) need generics. The pattern already works in plain code
  (see `tests/ok/arena.nge`): a struct holding an array and a length, with integer indices.
- The emitted C relies on what GCC and Clang do for things the C standard leaves implementation-defined:
  converting an out-of-range value to a signed type (`as`, and `<<`), `>>` of a negative value, and `__asm__`
  labels for the extern and export names. 16-bit targets (16-bit `int`) are not supported.
- Functions that are macros or inline assembly in C (CMSIS `__disable_irq()`, `__WFI()`) need a small C wrapper.
- Dropping a non-`copy` value without using it is allowed (there are no destructors yet).
- `let x: T;` (assign later) is not supported, only `var x: T;`.
- Recursion is not restricted yet.
- The compiler stops at the first error, and the only backend is C.
- Enums and pattern matching, generics, separate compilation.
