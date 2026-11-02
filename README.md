# Noge

A small systems language for embedded targets: memory safety first, fast compiles second.
The compiler (`nogec`) is written in C99 and emits C. See [docs/design.md](docs/design.md).

```
make          # builds build/nogec
make test     # runs tests/ok (compile, build, run, compare) and tests/err (must be rejected)

build/nogec -o out.c examples.nge && cc -o out out.c && ./out
```

## Milestone 1 status

Working: lexer, parser, type checker, C emitter.

Language so far: `i8..i64`, `u8..u64`, `bool`; `struct` (by value, nested); functions with
`let` / `inout` / `sink` parameters; `let` / `var`; `if` / `else if` / `else`; `while`; `return`;
arithmetic, comparison, `&&` `||` `!`; `as` casts between integer types and from `bool`;
struct literals; field access and field assignment; builtin `print(i32)`.

`inout` arguments are written `&place` at the call site and become C pointers.

## Known gaps (planned milestones)

- No definite-initialization, move or exclusivity checks yet: `sink` is currently a plain copy and
  `f(&a, &a)` is accepted. (milestone 2)
- Integer overflow, division by zero and out-of-range shifts are C undefined behaviour for now,
  and there are no arrays or bounds checks. (milestone 3)
- Evaluation order of call arguments and binary operands follows C (unspecified). Lowering to an IR
  with explicit temporaries will fix this. (milestone 3)
- The compiler stops at the first error.
- The AST uses arena-allocated pointers rather than the flat index arrays described in the design
  notes; this can change without affecting the language.
