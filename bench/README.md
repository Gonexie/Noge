# Noge against Rust

Two questions, measured separately: how fast do the programs run, and how long do they take to build? The same
algorithms are written in both languages, with the same checks switched on (arithmetic overflow and array bounds
checks), and a time is only reported if every build of a program prints the same output.

```
make bench                              # both benchmarks (about 20 minutes; leave the machine alone while it runs)
python3 bench/run.py --opt 2 --runs 7   # run time only
python3 bench/compile_bench.py --functions 4000 --modules 20      # build time only
```

Raw output of the runs below is in `results/`, with the machine and compiler versions (`results/machine.txt`).
Needs `rustc`, `gcc`, `clang` and `tcc`. The numbers come from one 4-vCPU container, so read ratios, not seconds.

## Short answer

- **Run time: about the same.** Noge compiles to C, so its speed is the C compiler's. Built with clang (the LLVM back end
  that rustc also uses) the eight programs run in 1.02 times Rust's time on average (0.87 to 1.17 on single programs).
  Built with gcc they take 1.11 times Rust's (0.91 to 1.30). That is a difference between back ends on a few loops, not
  between languages, and Noge is not faster than Rust here.
- **Build time of an optimized build: about 3 times less CPU, and the same wall time unless the program is split into files.**
  For 84,000 lines, `gcc -O2` takes 43 s of CPU and rustc 135 to 140 s. rustc spreads its 140 s over four cores and
  finishes in 43 s, exactly when gcc does on one core; with the program split into modules, `nogec --emit-dir` and
  `make -j4` finish in 10 s.
- **Unoptimized and incremental builds: not a win.** rustc's debug build (6 s of CPU) is cheaper than `gcc -O0` on Noge's C
  (15 s). After changing one function Noge rebuilds in 2.4 s and rustc (incremental, optimized) in 4.8 s, but rustc
  at opt-level 0 takes 2.1 s, and I did not measure Noge's `-O0` rebuild.
- **The front end is fast and the C compiler is not.** `nogec` needs 0.4 s for 84,000 lines; nearly all of the build is gcc or
  clang. With `tcc` the whole build takes 0.22 s, for slow code.
- **Executables:** 14.5 KB for Noge, 342 KB for Rust (std statically linked). That says nothing about code size in a real
  program, only about the runtime.

## Run time

`programs/NAME.nge` and `programs/NAME.rs` are the same algorithm written the same way (while loops, explicit
indexing). `NAME_idiomatic.rs` is what a Rust programmer would write (iterators, slices, `for`), for the three programs
where that could change the checks. Rust is built with `-C opt-level=N -C overflow-checks=on -C panic=abort
-C codegen-units=1`; Noge's C with `gcc` or `clang -O N`. Nobody is compiled for the machine it runs on. Best of 7 runs,
with the stack limit raised for both (the programs keep up to 20 MB arrays on the stack).

| program | what it does | noge+gcc | noge+clang | rust | gcc / rust | clang / rust |
|---|---|---:|---:|---:|---:|---:|
| collatz | longest chain below 3 million, `u64` | 0.852 | 0.880 | 0.839 | 1.02 | 1.05 |
| crc32 | table CRC-32, 8 passes over 16 MB | 0.367 | 0.365 | 0.354 | 1.04 | 1.03 |
| heapsort | 1 million `u32`, by hand | 0.168 | 0.151 | 0.129 | 1.30 | 1.17 |
| lcs | 15,000 x 15,000 dynamic program | 1.056 | 0.984 | 0.897 | 1.18 | 1.10 |
| matmul | 500 x 500, checked `u64` | 0.087 | 0.069 | 0.067 | 1.30 | 1.03 |
| pcg | 200 million PCG32 draws, `std/rand.nge` | 0.290 | 0.308 | 0.320 | 0.91 | 0.96 |
| sieve | primes below 20 million | 0.263 | 0.245 | 0.253 | 1.04 | 0.97 |
| vm | enum + `match` bytecode loop, 200 M instructions | 0.374 | 0.284 | 0.325 | 1.15 | 0.87 |
| **geometric mean** | | | | | **1.11** | **1.02** |

Seconds at `-O2` / `opt-level=2`. At `-O3` the means are 1.12 and 1.01 (`results/run_O3.txt`). The idiomatic Rust versions
are within 8 % of the plain ones (matmul 0.070 against 0.067, sieve 0.233 against 0.253, crc32 the same): LLVM already
removes most of the bounds checks in these loops, or they cost little.

Where Noge is behind with gcc (heapsort, matmul, lcs, vm), clang closes all of the gap on matmul and vm and part of it on
heapsort and lcs, so it is mostly the back end. I did not look at the machine code to say why.

## Build time

One program generated in both languages: N functions in M modules, each with a loop, a struct, an enum and `match`, an
if/else and a call to the previous function (so nothing is dead). The Rust program is one crate with the modules inline.
Both print the same number, and so do the edited versions. Best of 3.

| 84,000 lines (4,000 functions) | wall s | cpu s | peak MB |
|---|---:|---:|---:|
| `nogec` (front end) | 0.40 | 0.39 | 235 |
| + `gcc -O0`, one C file | 15.3 | 15.1 | 464 |
| + `gcc -O2`, one C file | 43.1 | 42.7 | 458 |
| + `clang -O2`, one C file | 39.1 | 38.7 | 546 |
| + `tcc` | 0.22 | 0.22 | 37 |
| `--emit-dir` + `make -j4`, `gcc -O0` | 4.7 | 16.2 | |
| `--emit-dir` + `make -j4`, `gcc -O2` | 10.3 | 38.5 | |
| one function edited, `--emit-dir` + `make`, `gcc -O2` | 2.4 | | 1 of 22 files recompiled |
| `rustc` opt-level 0 | 4.8 | 6.3 | 526 |
| `rustc` opt-level 2 (16 codegen units) | 42.7 | 140.0 | 625 |
| `rustc` opt-level 2, 1 codegen unit | 136.7 | 135.3 | 570 |
| one function edited, `rustc -C incremental`, opt-level 0 | 2.1 | | |
| one function edited, `rustc -C incremental`, opt-level 2 | 4.8 | | |

At 21,000 lines (1,000 functions, `results/compile_1000.txt`) the picture is the same: 9.4 s of CPU for `gcc -O2` against 31.7 s
for rustc, 3.0 s for the split build against 9.5 s, and 1.1 s against 2.0 s for an edit at `-O2`.

## What this does not show

- **The programs are small and plain.** Eight integer loops and one generated file of simple functions. No floating point
  (Noge has none), no strings, no traits, no closures, no heap. Real Rust code leans on exactly those, and rustc's
  trait solving and monomorphization are not exercised here: in this test LLVM is the bottleneck, which is where Noge has
  the least to offer. A big generic Rust program would probably build relatively worse, and nothing here shows that.
- **Noge is a much smaller language.** It checks ownership and initialization but has no lifetimes, traits or macros. The time
  saved is partly the work not done.
- **The run-time comparison is mostly gcc/clang against LLVM-in-rustc.** It says Noge does not give up speed by compiling to
  C with checks; it does not say the language makes programs faster.
- **One machine, a container, a few runs.** Differences under about 5 % are noise. gcc 13.3, clang 18.1, rustc 1.97 (clang's
  LLVM is not rustc's LLVM). The build wall times depend on having four cores.
- **Noge's `-O0` rebuild after an edit was not measured**, so that comparison with `rustc -C incremental` at opt-level 0 is missing.
- **Peak memory** is the largest process, taken separately from the timing runs.

## What the benchmark found

Writing it showed two real faults in the code Noge generates, now fixed:

1. `var a = [0; N]` was built in a temporary and then copied into `a`: twice the stack for every array initializer, and a copy.
   It is now built in place (`array_init_in_place` checks the emitted C).
2. A zero-filled array declared `T a = {0}` made gcc keep a zero-filled copy of the whole array in read-only memory and
   copy from it: a 16 MB executable for a 16 MB array, and on the Cortex-M3 at `-Os` 1,076 bytes of flash for a 1 KB array.
   Big arrays (over 64 bytes) are now cleared with a loop, which gcc and clang turn into `memset`: 50 bytes. The library
   firmware went from 4,964 to 4,872 bytes. `tests/codegen/` checks the size of the object file.

Both made the first measurements worse than the ones above, and neither shows in a test that only looks at output.
