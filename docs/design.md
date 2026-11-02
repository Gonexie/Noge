# Noge design notes

Goals, in priority order: (1) memory safety, (2) fast compiles, (3) low memory use and speed.
Target: embedded. Personal tool, so no ecosystem work for now.

## Notes carried over from the earlier conversation

**Safety model**
- Borrow checking is not what makes Rust slow to compile, so compile-time safety can stay.
- Start from Hylo-style value semantics: no lifetime variables, no reference types in the source language.
  Simpler to implement and check quickly. Cost: graphs, cycles and shared mutable observers are awkward;
  on embedded use indices into arrays or arenas for those anyway.
- Avoid Vale-style generational references: runtime checks plus a generation field per object work against goal 3.

**Fast compiles**
- LLVM took 84% of one project's release build: use a simple backend for debug builds, LLVM only for optimized release.
- Avoid full monomorphization where speed isn't critical.
- Small compilation units, fast linking.

**Speed and memory**
- No GC, no mandatory runtime. Explicit allocators/arenas, control over data layout.

**Personal tool**
- Skip ecosystem/tooling. Keep C interop (vendor libraries are C).

**First step:** tiny core language compiled to C.

## Core language sketch (v0)

Value semantics only. No references, no lifetimes, no pointers in safe code.

Types
- Integers `i8 i16 i32 i64 u8 u16 u32 u64`, `bool`, `unit`
- `struct` (fields are owned values), `enum` with payloads
- Fixed arrays `[T; N]` (N a compile-time constant)
- Index handles for arenas: `Id<T>` is a distinct integer type, `Arena<T, N>` is a fixed-capacity array plus length

Bindings
- `let x = e` immutable, `var x = e` mutable. Assignment copies or moves (see below).

Parameter conventions (Hylo-style, no reference types)
- `let`   read-only access, callee cannot mutate or keep it
- `inout` mutable access, caller's value is changed in place
- `sink`  ownership transferred; caller's binding is dead afterwards
- `set`   uninitialized in, initialized out (constructors)

Rules checked by the compiler
1. Definite initialization: no use of an unset or moved-from binding.
2. Move tracking: `sink` and last-use moves kill the source binding. Types are copyable only if marked `copy`.
3. Exclusivity: within one call, an `inout` path may not overlap any other argument path
   (`f(inout a, let a)` and `f(inout a.x, inout a.x)` are errors; `a.x` and `a.y` are fine).
4. Bounds: array/arena indexing is bounds-checked at runtime, unless the index is provably in range.
5. Integer overflow: checked by default (trap), with explicit `wrapping_add` etc. Division by zero traps.
6. No recursion in v0 (stack use is then statically boundable), optional later.

Unsafe boundary
- `extern "C" fn` declarations for vendor libraries. Calls to them are only allowed inside `unsafe { }`.
- Raw pointers: the type may appear anywhere, but reading, writing and offsetting through one needs `unsafe { }`
  (this replaces the first sketch's "the type only exists inside unsafe"; see the milestone 4 decisions).

Example
```
struct Counter { n: u32 }

fn bump(c: inout Counter) { c.n = c.n + 1; }

fn main() -> i32 {
    var c = Counter { n: 0 };
    bump(&c);      // `&` marks an inout argument at the call site
    return c.n as i32;
}
```

## Compiler layout

Single pass pipeline, each stage a separate small module, all data in flat arrays indexed by ints
(cheap to allocate, cache friendly, fast to compile).

```
src/
  lexer      bytes -> tokens
  parser     tokens -> AST (arena allocated, index based)
  resolve    names -> declarations, module/import graph
  types      type checking, struct layout, no generics in v0
  flow       init + move + exclusivity checks on a CFG (the safety pass)
  lower      AST -> small three-address IR with explicit checks (bounds, overflow)
  emit_c     IR -> C99, one .c file per module
  driver     CLI, invokes the C compiler, incremental by module hash
```

Notes
- Implementation language for the compiler: **C (C99)**. Fast builds, no dependencies, and the same toolchain that compiles the emitted code. Self-hosting is a later goal.
- Emit C that is plain and warning-free: structs as structs, `inout` as pointers, checks as inline `if (...) trap();`.
- `trap()` is a configurable hook, so an embedded target can reset or log instead of aborting.
- Incremental: hash each module's interface plus body, recompile only changed modules, link with the system linker.
- Generics (v1): start with monomorphization limited to small instantiation counts, revisit with a dictionary-passing mode.

## Milestones
1. Lexer, parser, type check for ints, structs, functions. Emit C, run hello-world on host. **Done** (see README).
2. Init/move/exclusivity pass, with a negative test suite of programs that must be rejected. **Done** (see below).
3. Arrays, bounds checks, overflow traps, an IR with a fixed evaluation order. **Done** (see below). Arena/handle types moved to the generics milestone.
4. C FFI with `unsafe`; build a blinky for a real target (e.g. ARM Cortex-M via arm-none-eabi-gcc). **Done** on an emulated Cortex-M3 (see below); not yet run on a physical board.
5. Enums and pattern matching. **Done** (see below).
6. Generics (and with them `Option<T>`, `Arena<T, N>`, `Id<T>`), then separate compilation.

## Decisions made in milestone 2

- **`copy` is opt-in**: `copy struct P { .. }`. All fields of a copy struct must be copy. Everything else moves.
  Scalars and `bool` are copy.
- **State is tracked per scalar leaf.** A struct is flattened into its scalar fields; a place like `r.size` is a
  contiguous range of leaves. Partial moves and field-by-field initialization need no special cases, and moving a
  field out of an `inout` parameter is legal as long as it is filled in again before every return.
- **Structured flow, no CFG.** The language has only `if`, `while` and `return`, so the pass walks the tree.
  `while` is iterated to a fixpoint (entry meet end-of-body) with errors suppressed, then analysed once more with
  errors on. Code after `return` is unreachable and not reported on.
- **`set` mode added** (callee must initialize; caller may pass an initialized or uninitialized place).
  `var x: T;` declares without initializing; `let` always needs an initializer, because "assigned exactly once"
  would need a second analysis.
- **Exclusivity started out stricter than Hylo's** (any two sibling operands that overlapped had to be reads,
  because C leaves evaluation order unspecified). Milestone 3 fixes the order, so it is now Hylo's rule.
- **`sink` parameters are immutable bindings** (take `var y = x;` to modify). `let` parameters cannot be moved from.
- Declared-but-unassigned variables are zero-initialized in the emitted C, so a bug in the analysis cannot read
  garbage and optimizers do not warn about maybe-uninitialized reads.
- Dropping a non-copy value unused is allowed for now. Revisit when types with destructors or linear resources exist.

## Decisions made in milestone 3

- **An IR between the AST and C** (`src/ir.h`): structured three-address code, so `if` and `while` stay nested and
  every run-time check is an explicit instruction. Lowering decides the evaluation order (left to right; an
  assignment evaluates its right side first, then the left place), so the C we print has one operation per
  statement and never depends on C's unspecified order. A variable operand is only copied to a temporary when a
  later sibling contains a call that could change it.
- **Exclusivity is Hylo's rule**: `let`, `inout` and `set` arguments that name a place borrow it from their argument
  until the call returns; a `sink` argument is taken when evaluated. An access conflicts with a live borrow it
  overlaps unless both are reads. This allows `q.x + bump(&q)` and `f(g(&a), &a)`, which milestone 2 rejected.
- **An array is one unit for flow analysis** (a single leaf): its elements are chosen at run time, so an element can
  be neither partially initialized nor moved out, and two element borrows always count as overlapping. A write to
  an element needs a whole array; `[v; N]` needs a copy element type.
- **Arrays are wrapped in a C struct** (`struct arr_i32_4 { int32_t v[4]; }`), so they can be assigned, passed and
  returned by value like any struct. `let` parameters of struct or array type are passed as `const T *`; `sink`
  parameters stay by value because the caller's copy may change before the callee runs.
- **Run-time checks are inline statements, not helper calls** (no call overhead at `-O0`, and each site carries its
  own line number). Overflow uses `__builtin_*_overflow`; `--no-builtins` prints widened or pre-condition tests
  instead. The failure hook is `NOGE_TRAP(kind, line)`, overridable at C compile time.
- **Shifts and `as`** work on the unsigned bit pattern: `<<` drops shifted-out bits, `>>` is arithmetic for signed
  types, both trap on a bad count; `as` truncates or sign-extends. `wrapping_*` are operators with modular results.
- **`len(a)` is a constant folded by the checker** and takes its integer type from context, so `i < len(a)` works
  for any index type. A literal index, zero divisor or shift count that is certainly wrong is a compile error.
- **Arenas are a library pattern for now.** `Arena<T, N>` and `Id<T>` need generics; a struct with an array and a
  length does the job today (`tests/ok/arena.nge`).
- **Dictionary lookups use hash tables.** The profile of a 68,000-line program showed 47% of the time in linear
  name lookups, which were quadratic in the number of declarations.

## Decisions made in milestone 4

- **`unsafe` guards use, not the type.** The first sketch said the raw pointer type exists only inside `unsafe`. That
  would forbid a driver struct with a `*mut u32` field or a safe wrapper that returns a pointer, and it protects
  nothing: a pointer value can do no harm until it is read, written or offset. So (as in Rust) pointer types, casts
  and comparisons are safe, and `ptr_read`, `ptr_write`, `volatile_read`, `volatile_write`, `ptr_offset` and calls
  to `extern` functions need an `unsafe` block. Intended style: put the raw access in two tiny safe wrappers
  (`reg_read`, `reg_write`) and use only those.
- **There is no address-of operator.** A raw pointer therefore cannot alias a Noge variable, and the borrow rules
  cannot be bypassed from `unsafe`. Giving C access to a Noge variable is done with an extern parameter mode
  (`inout`, `let`, `set`), which the flow pass checks like any other call: arrays travel as a pointer to their
  first element, `let` structs as `const T *`, `sink` structs by value.
- **The C boundary is a link-level contract.** Extern functions are declared in the emitted C under a private name
  (`x_strlen`) with an assembler label giving the real symbol, and exports (`f_tick`) the same in the other
  direction. So nothing can clash with a libc header or with generated names, and no vendor header is included.
  Struct layouts must agree with the C side field for field. This relies on GNU `__asm__` labels, like the
  `__builtin_*_overflow` default; both are fine for the GCC/Clang toolchains embedded work uses.
- **Constants and a real constant folder.** `const NAME: T = expr;` is an integer or bool, resolved lazily in any
  order with cycle detection; its initializer sees only other constants (no locals, no calls). Folding applies to
  every expression made of literals and constants, with the run-time semantics (checked arithmetic, shift counts,
  division), so what would trap at run time is a compile error. That closes the milestone 3 gap ("`255 + 1` traps
  at run time"). Array lengths are constant expressions. The folder is checked against run-time evaluation over
  21,844 operand combinations (`tests/arith_check.py`), and the check was mutation-tested: four deliberate bugs in
  the folder were each caught.
- **`usize` / `isize` follow `--ptr-bits`.** The compiler needs the target's pointer width to range-check address
  literals and to type `size_t`-like C parameters, so it is an option (`32` or `64`) and must match the C compiler
  used for the output. 16-bit targets are out of scope: a 16-bit `int` changes C's promotion rules and nothing has
  been tested there.
- **Freestanding mode** (`--freestanding`, or `--target=cortex-m` which adds 32-bit pointers) emits only
  `<stdbool.h>` and `<stdint.h>`, drops `print`, and routes failed checks to `noge_trap_hook(kind, line)`, a weak
  function that loops forever unless the board defines it. The generated code can still make the compiler call
  `memcpy` / `memset` / `memmove`, so a libc such as newlib-nano (or hand-written versions) is linked.
- **`inout`, `sink`, `set` and `copy` are contextual keywords.** The first real register code wanted a parameter
  named `set`. They are keywords only where a parameter's mode or `copy struct` can appear.
- **Literals for C.** `c"text"` is a `*const u8` to static NUL-terminated bytes; there are no other string types
  yet. Numbers accept `0b...` and digit separators because register code is full of them.
- **Interrupt handlers are `export "C" fn`.** No new mechanism: the function gets the plain C name and replaces the
  weak default in the vector table. Because there are no global variables, shared state is C-owned memory reached
  through a volatile pointer, as in `examples/cortex-m/systick.nge`.
- **Verification is by emulation.** The firmware runs under QEMU (`stm32vldiscovery`, a Cortex-M3). QEMU does not
  model the STM32 GPIO and RCC blocks, but logs every access to them, and the tests assert that exact register
  traffic. Results were identical at `-O0` / `-O1` / `-O2` / `-Os` and with the portable `--no-builtins` checks. The
  firmware has **not** been run on a physical board, so real-silicon behaviour (clock tree, LED polarity, timing)
  is unverified.

## Decisions made in milestone 5

- **An enum is a tagged union.** In C: `struct e_Shape { uint8_t tag; union { struct { int32_t f0; } v_Circle; ... } u; }`.
  Values live where they are put (stack, struct field, array element), cost the tag plus the largest variant, and
  need no allocation. The tag is the smallest of `u8`, `u16`, `u32`, `i32`, `i64` that holds every variant's number,
  and it *is* that number, so a plain enum casts to an integer for free. A variant is built with a compound literal
  (`(e_Shape){ .tag = 1, .u.v_Rect = { .f0 = w, .f1 = h } }`), which zeroes the bytes it does not use.
- **An enum is one unit for the flow pass, like an array.** Which variant it holds is a run-time fact, so a value
  inside it can be neither partly initialized nor partly moved. A pattern that binds a non-`copy` value therefore
  moves the whole matched variable (in that arm), and it has to be assigned before it is used again; binding only
  `copy` values, or nothing, just reads it. The state after a `match` is the join of the states at the ends of the
  arms. Moving out of a `let` parameter or an array element is refused, as everywhere else.
- **Patterns: `_`, names, integer and bool literals (negative too), `Enum::Variant(patterns)` to any depth, and `|`.**
  That is what state machines, decoders and interpreters need, and it keeps the exhaustiveness algorithm small
  enough to test thoroughly. Left out on purpose, to be added when a program needs them: guards (`if`), ranges,
  struct patterns, `if let` / `while let`, `@` bindings. Alternatives cannot bind names (which of them matched
  would decide whether the name has a value). A bare name that is also a variant of the matched enum is an error
  rather than a new variable, because writing `Circle` for `Shape::Circle` would otherwise match everything.
- **Exhaustiveness is checked by specializing a matrix of patterns** (the usual algorithm: rows are arms, columns
  are the values still to be looked at; for each variant, keep the rows that can match it and replace the first
  column by the values it carries). The error names the variants that are not fully covered. Integers can only be
  covered by a catch-all arm, so a `match` on a `u8` with 256 literal arms still wants a `_`: the check is
  conservative, never lenient. A row that fits everything ends the search (without that, a contrived match with 22
  refutable columns took 22 seconds to check and each four more columns multiplied it by 16).
- **An arm that can never match is an error**, not a warning: the compiler has no warnings, and a dead arm (a `_`
  too early, a repeated variant, a `_` after every variant) is nearly always a mistake. It is the same walk over the
  same matrix, asking whether the arm fits any value the arms above it leave over.
- **`match` as a statement or a value.** At the start of a statement it is a statement (arms are blocks or
  expressions, values dropped); anywhere else it is an expression whose arms are expressions of one type. The one
  block allowed in an expression is one that ends in `return`, so `let n = match o { Some(n) => n, None => { return -1; } };`
  works. The matched expression is evaluated once, before any arm.
- **Plain enums** (no variant carries values) cast to integers with `as` and compare with `==` / `!=`. Enums that
  carry values do neither; they are taken apart with `match`. `enum` is not `copy` unless declared `copy enum`
  (all carried values must then be `copy`), the same rule as for structs.
- **Operators only read their operands.** `==`, `!=`, `as`, `!` and arithmetic used to treat reading a non-`copy`
  place as a move, which never mattered while only integers and bools could appear there. A plain enum without
  `copy` that was compared or cast could not be used again. Found by the first test that compared one.
- **Enums stay out of C.** An enum, or a struct or array that holds one, cannot be a parameter or result of an
  `extern` or `export` function: C code could write a tag that names no variant, and a `match` would then read the
  values of the wrong one. Pass `variant as u32`.
- **No enum contains itself by value**, directly or through a struct or another enum; the checker says so. Recursive
  data needs indices into an array (see `tests/ok/arena.nge`) until there are generics and an arena type.
- **`match` lowers to flat code, not to a C `switch`.** The matched value is read where it lives (or evaluated once
  into a temporary). Each arm after the first sits in `if (pending)`; its pattern becomes a chain of nested `if`s on
  tag and literal comparisons, with the arm's code in the innermost one and `pending = false` at its end. A pattern
  with `|` sets a flag instead, so the arm is not repeated for each alternative. Flat means a 2000-arm `match` is not
  2000 blocks deep (C only guarantees 127). Every arm keeps its test, even the last one of an exhaustive match, so a
  tag that names no variant (only possible through unsafe code or a compiler bug) runs no arm instead of reading the
  wrong variant's values. A `switch` on the tag for the common case is an obvious later optimization; whether
  it would shrink or speed up the code has not been measured.
- **A function whose paths all return through arms of a `match` ends with a `return` that never runs.** The checker
  knows every path returns, but a C compiler cannot follow that through the `pending` flags and warns that control
  reaches the end of a non-void function (an error under `-Werror`, which the tests use).
- **`enum` and `match` are now reserved words.** A program that used them as names has to rename them.
- **Cost.** The generated C for a `match` is about 10 lines per arm. A 2000-variant enum with a 2000-arm `match`:
  `nogec` 0.17 s, 19,000 lines of C, 0.02 s for tcc, 1.3 s for `gcc -O0`, 3.0 s for `gcc -O2`.

### How milestone 5 was checked

- **Tests.** 8 programs that must run (`tests/ok/enum_*.nge`, `match_*.nge`: results worked out by hand, not copied
  from the compiler), 63 that must be rejected (`tests/err/`, each with the message it must give, and each message
  read to confirm it fires for the reason intended), a freestanding one, and `examples/cortex-m/machine.nge` (a
  stack machine and a traffic light, run under QEMU at `-O0`, `-O1`, `-O2`, `-O3`, `-Os` and with `--no-builtins`).
  Writing them, and the checks below, found five problems: operators moving their operands, a C-compiler warning for
  functions that end in a `match`, another for a `match` with one arm (a flag that was set and never read), a struct
  holding an enum being allowed across the C boundary, and an exhaustiveness search that took exponential time.
- **A differential test** (`tests/match_check.py`, `make check-match`) generates random enums (nested, with bool,
  integer and enum values, explicit and negative discriminants) and random arm lists, then compares the compiler with
  two independent models: a brute-force enumeration of every value of the scrutinee's type (verdict: accepted, not
  exhaustive, dead arm) and a small interpreter of match semantics (the arm taken and the values its names are bound
  to, compared with what the compiled program prints, built with `-Wall -Wextra -Wpedantic -Werror`). More than
  40,000 random programs were run, in default and portable mode, with gcc (`-O0`, `-O2`, `-Os`), clang and tcc.
  They all agree.
- **Mutation testing.** 37 deliberate bugs were put into the exhaustiveness, dead-arm and code generation code, one
  at a time; 35 are caught by the differential test, and 2 change speed but not behaviour (a loop over `true` and
  `false` that is symmetric, and testing the alternatives of a `|` pattern again after one has matched). In the first
  run (30 mutants) five were missed: two of those were the equivalent ones, three were real gaps in the generator
  (it never wrote explicit discriminants, and never made a variant with two refutable values). It was extended and
  they were caught. The lowering was then rewritten for smaller output and its 16 mutants were run against the new
  code, as were 2 for the shortcut in the search. The other checker mutants were last run against the generator as
  it was before it was extended.
- **Sanitizers and fuzzing.** The whole test suite and the differential test were run through a compiler built with
  AddressSanitizer and UBSanitizer, as were about 50,000 damaged programs (`tests/fuzz.py`: a token deleted, repeated,
  swapped or replaced, or the file cut short), the last 13,000 against the final code: no memory error, no crash,
  and every failure was an ordinary `error:` message. (The fuzzer itself was checked against fake compilers that
  crash and that print odd messages.) The C generated for every `tests/ok` program was built with `-Werror` at
  `-O2` and `-Os` with gcc and clang, and at `-O0` with gcc's sanitizers, in both overflow modes: 320 builds, no
  warning, the expected output every time. All of it passes through tcc as well.

What this does **not** show: nothing here ran on real hardware, on a 32-bit host or on a big-endian one; clang's
sanitizers were not installed here, so that one combination was not tried; the random programs use `copy` enums
holding bool, integer and enum values, so moves of non-`copy` values, structs inside variants, arrays and `inout`
scrutinees are covered by hand-written tests only; and the exhaustiveness search is exponential in the worst case,
which the shortcut above avoids for rows that fit everything but not for every contrived set of patterns.

## Open questions
- Do we need Hylo-style `subscript` projections to access into containers without copying, or is `inout` on paths plus arenas enough?
- Copy vs move default for small structs, and for plain enums: `copy enum Color { .. }` has to be written, and
  nearly every enum without values is wanted that way.
- Integer patterns: ranges (`0..=9`) and a check that a `match` on a `u8` with all 256 literals is exhaustive.
- Guards (`pattern if cond`) and `if let`: wait for a program that is clumsy without them.
- A `switch` on the tag in the generated C for the common simple `match`.
- Interrupt handlers and shared state with main code: `export "C"` handlers and C-owned memory work today, but a
  typed answer (global `static`s, a `Volatile<T>` or critical-section type) is still to be designed.
- A safe way to hand C a pointer to a Noge buffer that outlives one call (today only extern parameter modes, for
  the duration of the call).
- A pointer-to-first-element intrinsic for arrays (`as_ptr`) would shorten C string and buffer code, at the price of
  a pointer that the flow pass cannot track.
