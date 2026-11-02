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
- Raw pointer type exists only inside `unsafe`.

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
2. Init/move/exclusivity pass, with a negative test suite of programs that must be rejected.
3. Arrays, arenas, bounds checks, overflow traps.
4. C FFI with `unsafe`; build a blinky for a real target (e.g. ARM Cortex-M via arm-none-eabi-gcc).
5. Enums and pattern matching; then generics.

## Open questions
- Do we need Hylo-style `subscript` projections to access into containers without copying, or is `inout` on paths plus arenas enough?
- Copy vs move default for small structs.
- Interrupt handlers and shared state with main code: needs its own rule (likely a `Volatile`/critical-section type in unsafe-wrapped library code).
