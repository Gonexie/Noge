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
5. Enums and pattern matching; then generics.

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

## Open questions
- Do we need Hylo-style `subscript` projections to access into containers without copying, or is `inout` on paths plus arenas enough?
- Copy vs move default for small structs.
- Interrupt handlers and shared state with main code: `export "C"` handlers and C-owned memory work today, but a
  typed answer (global `static`s, a `Volatile<T>` or critical-section type) is still to be designed.
- A safe way to hand C a pointer to a Noge buffer that outlives one call (today only extern parameter modes, for
  the duration of the call).
- A pointer-to-first-element intrinsic for arrays (`as_ptr`) would shorten C string and buffer code, at the price of
  a pointer that the flow pass cannot track.
