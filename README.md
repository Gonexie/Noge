# Noge

A small systems language for embedded targets: memory safety first, fast compiles second.
The compiler (`nogec`) is written in C99 and emits C. See [docs/design.md](docs/design.md).

```
make              # builds build/nogec
make test         # host tests, then the Cortex-M tests under QEMU if the ARM toolchain is installed
make check-arith  # slower: the overflow/division/shift code and the constant folder, against __int128
make check-match  # slower: random enums and match arms, against a brute-force model and a reference interpreter
make check-generics  # slower: random generic programs, against the same programs with every instance written out
make check-modules   # every positive test cut into two files, against its expected output
make check-separate  # the same programs built from one C file per module, and what a rebuild touches
make check-std    # the standard library against independent models in Python (zlib, exhaustive 8-bit tables, ...)
make fuzz         # damaged programs must be rejected cleanly, never crash the compiler
make -C examples/cortex-m test   # just the firmware (see examples/cortex-m/README.md)

./noge prog.nge                    # compile and run in one step (builds the compiler the first time)
./noge -O prog.nge                 # the same with gcc -O2: slower to build, faster to run
./noge --build-dir=build prog.nge  # one C file per module in build/; the next run rebuilds only what changed

build/nogec -o out.c prog.nge && cc -o out out.c && ./out      # the same thing, by hand
build/nogec --target=cortex-m -o fw.c fw.nge      # for a microcontroller: no C library, 32-bit pointers
```

## Running a program: `./noge`

`./noge my/hello.nge` builds the compiler if it is not built yet, compiles the program and runs it, then deletes
the temporary files. The program's exit status is passed through (a failed run-time check gives 101); a program
that does not compile gives status 1. If `tcc` is installed it is used for the C, which is far faster than gcc
(the generated C for a 68,000-line program: 0.18 s against 19.8 s); `--gcc` forces gcc, `-O` uses `gcc -O2`, and
`--emit-c` prints the generated C instead of running anything. It handles programs of several `.nge` files; a program
that calls your own C files needs the three steps by hand (`nogec`, a C compiler with all the files, run).

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

Items: `fn`, `unsafe fn`, `struct`, `copy struct`, `enum`, `copy enum` (any of the first six can be generic), `const`,
`static`, `extern "C" fn`, `export "C" fn`, and `import` of another file. Any item can be marked `pub`.
Types: `i8..i64`, `u8..u64`, `usize`, `isize`, `bool`, `struct` and `enum` (by value, nested), fixed arrays `[T; N]`,
raw pointers `*mut T` and `*const T`. Statements: `let`, `var`, `if` / `else if` / `else`, `while`, `match`, `return`,
assignment, `unsafe { }`. Expressions: arithmetic `+ - * / %`, bitwise `& | ^ ~ << >>`, comparison, `&& || !`, `as` casts (between
integers, from `bool`, between integers and pointers, between pointers), struct literals, array literals `[a, b, c]`
and `[value; N]`, field access, indexing, C strings `c"text"`. Numbers may be written `255`, `0xFF`, `0b1111` and
with separators, `0x4002_1018`. A character literal such as `'a'` is a number too, the ASCII code of the character
(97): it takes its type from its context like any other number, so `let c: u8 = 'a';` and `if c >= '0' && c <= '9'`
work. The escapes are `\n \r \t \0 \\ \' \" \xHH`; only ASCII characters can be written directly. Precedence follows Rust (`& | ^` bind tighter than `==`).

`inout`, `sink`, `set` and `copy` are keywords only where they can be (in front of a parameter's type, in front of
`struct` or `enum`); elsewhere they are ordinary names.

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

## Enums and `match`

An enum is a value that is exactly one of several variants, and a variant may carry values. `match` takes it apart,
and the compiler refuses a `match` that forgets a variant.

```
enum Shape {
    Circle(i32),                  // a variant can carry values
    Rect(i32, i32),
    Empty,
}

fn area(s: Shape) -> i32 {
    return match s {              // every variant must be handled
        Shape::Circle(r) => 3 * r * r,
        Shape::Rect(w, h) => w * h,
        Shape::Empty => 0,
    };
}

copy enum Color { Red, Green, Blue = 10 }   // `copy`: using it by value duplicates it

fn describe(n: i32) {
    match n {                     // as a statement the arms are blocks (or expressions)
        0 => { print(100); }
        1 | 2 => { print(200); }  // either
        -1 => { print(300); }
        x => { print(x); }        // a name takes the whole value
    }
}

fn doubled(s: Shape, fallback: i32) -> i32 {
    let n = match s {             // as a value every arm is an expression of one type ...
        Shape::Circle(r) => r,
        Shape::Rect(w, _) => w,
        Shape::Empty => { return fallback; }   // ... or a block that ends in `return`
    };
    return n * 2;
}
```

**Declaring.** `enum Name { A, B(T, U), C = 5 }` (a trailing comma is fine). Write a variant as `Name::A` or
`Name::B(1, 2)`. `copy enum` is like `copy struct`: every value a variant carries must be `copy`. Without `copy` an
enum is move-only, so a plain enum that is passed around by value is usually declared `copy`. A variant that
carries no values may be given a number (`C = 5`, any integer constant); the others count up from the variant before,
starting at 0. Enums nest: a variant can carry a struct or another enum, and an enum can be a struct field or an
array element (`[Op; 10]`; `[value; N]` needs a `copy enum`). An enum cannot contain itself by value: use an index
into an array, as in `tests/ok/arena.nge`. `enum` and `match` are reserved words.

**Plain enums.** An enum none of whose variants carries values converts to its number with `as`
(`Color::Blue as i32` is 10; it wraps like any other `as`) and compares with `==` and `!=`. An enum that carries
values does neither; take it apart with `match`.

**Patterns.** The arms of a `match` are tried from the top and the first one that fits is used.

| pattern                         | fits                                                                  |
|---------------------------------|-----------------------------------------------------------------------|
| `_`                             | anything                                                              |
| `name`                          | anything, and gives it that name inside the arm                       |
| `5`, `-1`, `true`               | that integer or bool                                                  |
| `Shape::Empty`, `Shape::Rect(w, h)` | that variant; inside the parentheses one pattern per value it carries, nested as deep as you like: `Msg::Pair(Cmd::Say(1 \| 2), _)` |
| `p \| q`                        | either; the alternatives cannot bind names, since the arm could not tell which of them had one |

A name in a pattern is a new variable that exists only in its arm. A bare name that is also a variant of the
matched enum (`Circle` instead of `Shape::Circle`) is an error, not a new variable, because that mistake would
silently match everything.

**A statement or a value.** At the start of a statement `match` is a statement: each arm is `pattern => { ... }` or
`pattern => expression,`, and an expression's value is dropped. Anywhere else it is an expression and gives the
value of the arm that fits: every arm is `pattern => expression,`, all of one type. In an expression the only
block allowed is one that ends in `return` (an early exit). A function whose arms all `return` needs no
`return` after the `match`.

**What the compiler checks.**

- *Exhaustive.* Every possible value must be fitted by some arm, to any depth. The error names what is missing:
  `match is not exhaustive: 'Shape::Empty' is not fully covered (add an arm for it, or a '_' arm)`. A `bool` needs
  `true` and `false`, or a catch-all. Integers can only be covered by a catch-all arm (`_` or a name): a `match` on a
  `u8` with all 256 literals still wants a `_`.
- *No dead arms.* An arm that cannot fit any value the arms above it leave over is an error
  (`this arm can never match`): a `_` first, a repeated variant, a `_` after every variant.
- *Moves.* A name that binds a `copy` value (an integer, a `copy enum`) takes a copy. A name that binds a value that
  is not `copy` moves it out of the matched variable, which then counts as moved until it is assigned again (the
  enum is one unit for the move analysis, since which variant it holds is only known at run time). That is not
  possible for a `let` parameter or an array element, so match those with `_` or bind only copy values.
  Matching a place and binding nothing, or only copy values, never moves it.
- *Order.* The matched expression is evaluated once, before any arm; only the arm that fits runs; the values of a
  variant are evaluated left to right.

**In C.** An enum is a struct with a tag and a union of the variants' values, so it lives where you put it (stack,
struct, array), costs the tag plus its largest variant, and needs no allocation. The tag is the smallest of `u8`,
`u16`, `u32`, `i32` and `i64` that holds every number given to the variants, and it *is* that number. A `match` is a
chain of tag comparisons. An enum cannot be passed to or returned from C, nor can a struct or array that holds one,
because C code could write a tag that names no variant; pass `variant as u32` instead.

Not in this version: guards (`pattern if condition`), ranges (`1..5`), patterns that name struct fields,
`if let` / `while let`, naming the whole value as well as its parts (`n @ Some(_)`), and names inside `|`.
Generic enums such as `Option<T>` work (see Generics), but there is no ready-made one: you write it.

## Generics

A struct, enum or function can take parameters that stand for a type, or for a number such as an array length.
Each use with particular arguments makes an *instance*: a copy of the declaration with the parameters replaced,
which is checked exactly like code written by hand. So every safety check (types, moves, exclusivity, run-time
checks) applies to every instance, and there is nothing generic left at run time.

```
copy enum Option<T> { Some(T), None }                  // T is a type
copy struct Pair<A, B> { a: A, b: B }
struct Ring<T, const N: usize> { items: [T; N], head: usize, count: usize }   // N is a number

fn unwrap_or<T>(o: Option<T>, d: T) -> T {
    return match o {
        Option::Some(v) => v,
        Option::None => d,
    };
}

fn ring_new<T, const N: usize>(fill: T) -> Ring<T, N> {
    return Ring { items: [fill; N], head: 0, count: 0 };
}

fn main() -> i32 {
    let o = Option::Some(5);                  // an Option<i32>: T is worked out from the 5
    print(unwrap_or(o, 9));                   // 5
    let r: Ring<u8, 4> = ring_new(0);         // T and N come from the type the result is wanted as
    let q = ring_new::<i32, 8>(0);            // or are written out
    return 0;
}
```

**Declaring.** `<T, U, const N: usize>` goes after the name of a `struct`, `enum` or `fn`. A `const` parameter has an
integer type and can be used as an array length (`[T; N]`) and as a value (`return N;`). The types are used as
`Option<i32>`, `Ring<u8, 4>`, `Pair<Option<u8>, bool>`; the numbers are a literal, a negative literal, or the name of a
`const`.

**Working out the arguments.** Nothing has to be written out when the compiler can tell, in this order:
the arguments written with `::<...>` (the first few is enough, the rest are worked out); the type the result is
wanted as (a `let` annotation, a field, a parameter, a `return`); the types of the arguments, where a plain number
goes last and takes the type another argument has (`f(1, x)` with `x: u8` makes `T` a `u8`); the length of an array
argument for `const N`. A value such as `Pair { a: 1, b: true }` or `Option::Some(5)` works out its own arguments
the same way. When something cannot be told, the error says which parameter and what to write: `Option::<i32>::None`,
`make::<u8>()`.

**`copy`.** `copy struct Pair<A, B>` means: copy when `A` and `B` are, as for `#[derive(Copy)]` in Rust. So `Pair<i32, i32>`
can be used again after `let q = p;`, and `Pair<Buf, i32>` moves like `Buf` does. A parameter of a generic function
that is moved from has to be `sink`, as anywhere else.

**Each instance is checked on its own.** There are no bounds (`T: Ord`): `a + b` inside a generic function is
accepted for the types where it works and is an error for the others, and the error carries a note saying which
instance and where it was asked for:

```
t.nge:3: error: operator '+' needs integer operands, found 'bool'
  note: in 'add<bool>', which was asked for on line 7
```

A generic function that nothing calls is not checked at all. Instances are made on demand, once per distinct set of
arguments; their names in the emitted C are `s3_Pair`, `e4_Option`, `f5_unwrap_or`.

**Limits.** More than 20,000 instances, instances nested more than 48 deep, and types whose names would pass 2000
characters are errors (they mean a generic function that calls itself with a bigger type each time). `main`, `extern`
and `export` functions cannot be generic. A type cannot contain itself by value, generic or not.

Not in this version: bounds and `where` clauses, default arguments, arithmetic in arguments (`Ring<T, N + 1>`), and a
standard library: write the `Option<T>` you need, as above, until there are modules.

## Modules and the standard library

A program can be several files. Each file is a module. The file you give `nogec` (or `./noge`) is the program's own;
it must have `main`. Other files are brought in with `import`, by a path relative to the importing file:

```
import "geom.nge" as geom;       // its public names are used as geom::name
import "util.nge";               // its public names are merged into this module's own

fn main() -> i32 {
    let p = geom::Point { x: 3, y: 4 };
    print(geom::dist2(p));                       // a function, a struct and an enum are reached the same way:
    let s: geom::Shape = geom::Shape::Circle(2); // module::Name, module::Enum::Variant
    return twice(21);                            // from util.nge, no prefix
}
```

```
// geom.nge
pub struct Point { x: i32, y: i32 }
pub enum Shape { Circle(i32), Empty }
pub fn dist2(p: Point) -> i32 { return p.x * p.x + p.y * p.y + helper(); }
fn helper() -> i32 { return 0; }               // not pub: only this file can use it
```

- **`pub`** on a `fn`, `struct`, `enum`, `const` (or `extern` / `export` function) lets other files use it. Anything
  else is private to its file, and using it from outside is an error that says so. The fields of a `pub` struct are
  all visible. Two files may each have a private `helper`, or a `pub struct Item`: names are per file.
- **`as name`** gives the module a name to write in front of its items. Without it the items are merged in, and the
  importing file's own declarations win over them; a name that two imports both provide is an error at the place it
  is used (write `module::name` with `as` to say which). A module name cannot also be a declaration's name.
- A file imported by several others is loaded once. Files cannot import each other in a circle.
- Generic code resolves the names in it where it was written: a generic function from `lib.nge` can call lib's private
  helpers when it is used from `main.nge`. A run-time failure and a compile error name the file they are in.
- By default everything is compiled into one C file. For a big program, see below: one C file per module.

**One C file per module.** `nogec --emit-dir=build prog.nge` writes `build/noge_types.h` (the types and the run-time
check machinery), a `.c` file for every module that has functions, and a `build/Makefile`; `make -C build` compiles
each file and links them (`CC`, `CFLAGS`, and `EXTRA` for more C files to link can be set on the command line).
`./noge --build-dir=build prog.nge` does all of that and runs the program. The whole program is still checked together
(it takes 0.1 s for 24,000 lines), so nothing else is needed to keep the modules in step; what is separate is the C.
A file is only rewritten when its text changes, and each file lists just the functions it defines and calls, so:

- changing a function's body rebuilds that module's file only; so does adding a function, or a new use of a generic
  function from another module (the instance is written into the generic function's own module);
- changing a struct or enum changes `noge_types.h`, which every file includes, so everything rebuilds, as in C;
- the C names of modules and instances come from hashes of what they are, not from how many came before, so one
  module's file does not change because another module got a new instance;
- changing the compiler or its flags rebuilds everything (the Makefile remembers them).

On a test project of 41 files, 24,000 lines and 2,400 functions, `gcc -O2` took 19.0 s for a full build (20.9 s as one C
file); after editing one function, `nogec` and `make` took 0.6 s together. The price is that a call from one module to
another cannot be inlined by the C compiler (`-flto` brings it back): the firmware example is 4 bytes bigger.

**The standard library** is written in Noge, in the `std/` folder next to the compiler (`--std-dir=DIR` or the
`NOGE_STD` variable say where it is if the compiler is moved). `import "std/ring.nge" as ring;` loads it. Nothing in
it allocates: containers have their size in the type, and text lives in fixed buffers. The whole library is plain
Noge, so it is checked, generic and portable like your own code, and a program only pays (in flash) for what it calls.

| file                | what it has                                                                              |
|---------------------|------------------------------------------------------------------------------------------|
| `std/prelude.nge`   | `Option<T>` and `Result<T, E>`: imported into every module, without asking (`--no-prelude` stops it) |
| `std/option.nge`    | `is_some`, `is_none`, `contains`, `unwrap_or`, `or`, `and`, `xor`, `flatten`, `take`, `replace`, `ok_or` |
| `std/result.nge`    | `is_ok`, `is_err`, `unwrap_or`, `unwrap_err_or`, `ok`, `err`, `or`, `and`                |
| `std/math.nge`      | `min`, `max`, `clamp`, `swap`, `abs`, `sign`, `abs_diff`, `div_ceil`, `rem_euclid`, `align_up`, `gcd`, `lcm`, `pow`, `isqrt`, `map_range`, and `checked_add` / `checked_sub` / `checked_mul` (an `Option` instead of a trap), for any integer type |
| `std/bits.nge`      | `popcount`, `clz`, `ctz`, `log2`, `next_pow2`, rotates, byte swaps and bit reversal for `u32` and `u64`; `get` / `set` / `clear` / `toggle` / `put` / `is_pow2` for any integer type; `field` / `set_field` for register fields |
| `std/array.nge`     | on the first `n` elements of an array: `sort`, `heap_sort`, `binary_search`, `reverse`, `rotate_left`, `index_of`, `contains`, `count`, `sum`, `min_index`, `max_index`, `is_sorted`, `insert`, `remove`, `copy`, `equal`, `fill`, `swap_at` |
| `std/vec.nge`       | `Vec<T, N>`: room for N elements: `push`, `pop`, `get`, `set`, `first`, `last`, `insert`, `remove`, `swap_remove`, `truncate`, `extend`, `append`, and the searching and sorting of `array` |
| `std/ring.nge`      | `Ring<T, N>`: a queue, and a double-ended queue: `push`, `pop`, `push_front`, `pop_back`, `push_overwrite`, `peek`, `peek_back`, `get` |
| `std/arena.nge`     | `Arena<T, N>` and the handle `Id<T>`: `new`, `alloc`, `get`, `set`, `try_get`            |
| `std/bitset.nge`    | `Bitset<W>`: 32·W flags: `set`, `clear`, `toggle`, `test`, `count`, `next_set`, `next_clear`, union / intersection / difference / xor, `is_subset` |
| `std/map.nge`       | `Map<K, V, N>`: up to N key-value pairs: `put`, `get`, `get_or`, `has`, `remove`, `key_at`, `value_at` |
| `std/ascii.nge`     | `is_digit`, `is_alpha`, `is_space`, `to_upper`, `to_lower`, `hex_value`, ... on a `u8`   |
| `std/str.nge`       | `Str<N>`: text in a buffer of N bytes: `push`, `append`, `append_cstr`, `eq`, `compare`, `find`, `starts_with`, `trim`, `to_upper`, `slice`, ... |
| `std/fmt.nge`       | numbers into a `Str` (`append_int`, `append_uint`, `append_hex`, `append_bin`, `append_fixed`, padded forms) and back (`parse_int`, `parse_uint`, `parse_hex`) |
| `std/io.nge`        | hosted programs only: `write`, `writeln`, `write_int`, `write_hex`, `newline`, `write_cstr` (through `putchar`) |
| `std/rand.nge`      | PCG32 random numbers: `seed`, `next`, `below` (no bias), `range`, `coin`, `shuffle`; `xorshift32` for tiny chips |
| `std/hash.nge`      | `crc32` (also a table-driven `crc32_fast`), `crc16_ccitt`, `crc16_modbus`, `crc8`, `adler32`, `fnv1a32`, `fnv1a64`, `xor8`; each also byte by byte |

Every file starts with a comment that says how to use it and what happens at the edges, and the functions are
documented where they are written, so `std/str.nge` is the reference for strings. A few conventions hold throughout:

- The containers hold `copy` types and are `copy` themselves (`new` takes the value the unused slots hold, since there
  are no uninitialized arrays). A program that declares its own `Option` or `Result` uses its own.
- Running out of room, or asking for something that is not there, gives a `false` or a `none`, never a trap. What does
  trap is what traps anywhere: arithmetic that overflows, a division by zero, an array index out of bounds, a shift by too
  much (a bit number out of range for `bits::set`, a bit index beyond a `Bitset`). The comment above each function says
  when.
- Anything that compares (`sort`, `contains`, `max`) is for numbers, `bool`s and enums without data; a struct is refused
  where it is used, with a note that says which std function and which line asked for it.
- There are no functions that take functions (no `map`, `filter`, `sort_by`): the language has no function values yet.
  Use a `match` or a `while`.

```
import "std/ring.nge" as ring;

fn main() -> i32 {
    var r: ring::Ring<u8, 4> = ring::new(0);
    ring::push(&r, 7);
    match ring::pop(&r) {              // Option comes from the prelude
        Option::Some(x) => { print(x as i32); }
        Option::None => { }
    }
    return 0;
}
```

**Text.** There is no string type in the language, only `c"..."` (a pointer to bytes that end in a 0) and byte arrays;
`std/str.nge` builds the useful part on top of them, and `'a'` is the number 97, so a character is a `u8`:

```
import "std/str.nge" as str;
import "std/fmt.nge" as fmt;
import "std/io.nge" as io;

fn main() -> i32 {
    var s: str::Str<48> = str::new();                     // text of up to 48 bytes, on the stack
    unsafe { str::append_cstr(&s, c"temperature = "); }   // reading a c"..." literal is unsafe: the pointer is trusted
    fmt::append_fixed(&s, -1234, 2);                      // -12.34, from a number of hundredths: no floats needed
    str::push(&s, ' ');
    str::push(&s, 'C');
    io::writeln(s);                                       // temperature = -12.34 C   (hosted programs)
    return 0;
}
```

On a microcontroller leave out `std/io.nge` and hand `s.bytes` (after `str::terminate(&s)`) to your own output routine, as
`examples/cortex-m/stdlib.nge` does: that program uses most of the library, without a C library, in 4,964 bytes of flash
and no RAM beyond the stack.

## Constants

`const NAME: T = expr;` declares an integer or bool known at compile time. Constants can use each other in any
order and can be array lengths, so `[u8; BUF_LEN * 2]` works. More generally, **any expression made only of literals
and constants is computed by the compiler**, with the same rules as at run time: `255 + 1` as a `u8`, `5 / (1 - 1)`,
`1 << 40` as an `i32` or an array index that is certainly out of range are compile errors.

## Statics

`static NAME: T = value;` is a global variable. Without `mut` it is read-only data, such as a lookup table: the C
compiler puts it in flash (it is `const`), and reading it is safe anywhere. `static mut` can be changed, and every use of
it needs `unsafe { }`, because the compiler cannot see who else uses it. `static mut volatile` is for what an interrupt
handler shares with the main program: every access really reads or writes memory.

```
static CRC_TABLE: [u32; 4] = [0, 0x1DB7_1064, 0x3B6E_20C8, 0x26D9_30AC];   // read-only data
static mut volatile TICKS: u32 = 0;                                        // shared with an interrupt handler

export "C" fn SysTick_Handler() {
    unsafe { TICKS = TICKS + 1; }
}

fn main() -> i32 {
    print(CRC_TABLE[2] as i32);      // safe: bounds-checked like any array
    unsafe {
        while TICKS < 10 { }         // waits for the handler (volatile: read again every time round)
    }
    return 0;
}
```

- **The initial value** is data known at compile time: numbers, bools, constants, and struct, array and enum values made of
  them (`[0; 256]`, `Pt { x: 1, y: 2 }`, `Option::Some(3)`), not calls. The type has to be `copy`, so reading a static
  copies it and nothing is ever moved out of one. Without `volatile`, a loop that waits for an interrupt handler may
  never end: the C compiler is free to keep the variable in a register (the SysTick firmware hangs at `-O2` without it).
- **Reading and writing** are places like any other: `TABLE[i]`, `POS.x = 3`. A `static mut` passed to a function whose
  parameter is a `let` array or struct is copied first, so the function keeps the value it was given even if the static
  changes while it runs; an immutable static is passed by address, with no copy. A `static mut` can be an `inout` argument
  (`fill(&BUF)`), a `volatile` one cannot (the C pointer would lose the `volatile`).
- **Not tracked:** the compiler does not check initialization, moves or overlapping borrows of statics, since the value is
  always set and other code may use it at any time. That is what `unsafe` is for.
- **A static is not a constant:** `[u8; LIMIT]` and other things needed at compile time want a `const`. `len(TABLE)` is a
  constant, though, and needs no `unsafe` even for a `static mut`.
- Statics can be `pub` and are reached as `module::NAME`; two modules can each have a `COUNT`. In C they are `g_NAME`
  (`gm<tag>_NAME` in other modules), and with `--emit-dir` the definition is in its module's file and an `extern` in the
  header.

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

**Unsafe functions.** `unsafe fn name(...)` declares a function the compiler cannot make safe by itself: calling it needs
an `unsafe { }` block (or another `unsafe fn`), and its own body counts as an unsafe block, so it can use the pointer
operations freely. It is how a library offers something like "read this C string" without letting safe code do it:

```
unsafe fn first_byte(p: *const u8) -> u8 { return ptr_read(p); }

fn main() -> i32 {
    unsafe { print(first_byte(c"A") as i32); }     // 65; outside the block: "calling the unsafe function 'first_byte' requires an unsafe block"
    return 0;
}
```

An `unsafe fn` can be generic and `pub`; `main` and `extern`/`export` functions cannot be `unsafe fn`.

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
| `--emit-dir=DIR`       | one C file per module in DIR, with a header and a Makefile (see Modules)         |
| `--std-dir=DIR`        | where `std/` is (or the `NOGE_STD` variable); `--no-prelude` leaves out `Option` / `Result` |

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
- **`match`.** Exhaustive, no dead arms, and the move rules for names bound in patterns. See "Enums and `match`".

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

- A `static mut` is the only way to share state with an interrupt handler, and it is unsafe to use; there is no
  type that makes sharing safe (a critical section or atomic type) yet.
- The emitted C relies on what GCC and Clang do for things the C standard leaves implementation-defined:
  converting an out-of-range value to a signed type (`as`, and `<<`), `>>` of a negative value, and `__asm__`
  labels for the extern and export names. 16-bit targets (16-bit `int`) are not supported.
- Functions that are macros or inline assembly in C (CMSIS `__disable_irq()`, `__WFI()`) need a small C wrapper.
- Dropping a non-`copy` value without using it is allowed (there are no destructors yet).
- `let x: T;` (assign later) is not supported, only `var x: T;`.
- Recursion is not restricted yet.
- The compiler stops at the first error, and the only backend is C.
- Modules have no re-exports
  (`pub import`), no `use` to bring one name in, and no nesting (`a::b::c`). Generics have no bounds. There are no
  function values or closures, so the standard library has no `map`, `filter` or `sort_by`, and no way to fail on
  purpose (`unwrap`, `assert`, `unreachable`) because the language has no `trap()`; `match` and the `Option` helpers
  stand in. There is no floating point (`fmt::append_fixed` prints fixed-point numbers) and no character type (a
  character is a `u8`). Text is a fixed-size `Str<N>` that you size yourself. Patterns have no guards, ranges, struct patterns, `if let` or `@` yet, and an enum cannot be passed to C.
