# Noge on a Cortex-M3

Six small programs that run on an ARM Cortex-M3, plus the C board support they need. They are built with
`nogec --target=cortex-m` (no C library, 32-bit `usize`) and `arm-none-eabi-gcc`, and run under QEMU.

```
make          # build everything into build/
make test     # run each program under QEMU and check what it did
make size     # flash and RAM use of each image
make OPT=-O2 test              # other optimization levels give the same results
make NOGEFLAGS=--no-builtins   # portable C99 overflow checks instead of __builtin_*_overflow
```

Needs `arm-none-eabi-gcc` and `qemu-system-arm` (on Debian/Ubuntu: `apt install gcc-arm-none-eabi
qemu-system-arm`). From the repository root, `make test` runs these tests too when the tools are there.

| program        | what it shows                                                                                   |
|----------------|-------------------------------------------------------------------------------------------------|
| `blinky.nge`   | toggles PC13 by writing the GPIO and SysTick registers; the whole point of the language          |
| `hello.nge`    | a `c"..."` string passed to C                                                                    |
| `checks.nge`   | CRC-32, 64-bit arithmetic with overflow checks, a sort, `usize` being 32 bits                    |
| `systick.nge`  | an interrupt handler written in Noge (`export "C" fn SysTick_Handler`) sharing a `static mut volatile` with `main` |
| `trap.nge`     | a failed bounds check on the device: the hook reports the kind and the source line               |
| `machine.nge`  | enums, `match` and generics: a stack machine, a traffic light, and a ring buffer whose size is a type parameter |

## The files

- `startup.c` — vector table and reset handler (copies `.data`, zeroes `.bss`, calls `main`). Weak default
  handlers; a Noge `export "C"` function with the right name replaces one.
- `link.ld` — 64 KB flash at `0x08000000`, 8 KB RAM at `0x20000000`.
- `board_qemu.c` — output and exit through ARM semihosting, the run-time check hook, a tick counter.
- `board_hw.c` — the same hooks for a real board: they do nothing, and a failed check halts the chip.

The Noge programs call the board code with `extern "C" fn`, inside `unsafe { }`.

## The blinky

`blinky.nge` is the same source on QEMU and on a board; only the C file linked next to it differs
(`blinky.elf` with `board_qemu.c`, `blinky-hw.elf` with `board_hw.c`). The raw memory accesses are confined to
two functions:

```
fn reg_write(addr: usize, value: u32) { unsafe { volatile_write(addr as *mut u32, value); } }
fn reg_read(addr: usize) -> u32 { unsafe { return volatile_read(addr as *const u32); } }
```

Everything else is ordinary checked code. The register addresses are `const`s, and expressions such as
`0xF << ((LED_PIN - 8) * 4)` are computed by the compiler.

The hardware image is **260 bytes of flash and no RAM**: 64 bytes of vector table, 64 of reset handler and
132 of program.

## What the tests check

QEMU's STM32 model does not implement the RCC and GPIO blocks, so it logs every access to them
(`-d unimp`). `test.sh` uses that log to check the blinky's register traffic exactly: the clock enable
(`RCC_APB2ENR` ← `0x10`), the pin configuration (`GPIOC_CRH` ← `0x00200000`), then six writes to `GPIOC_BSRR`
alternating `0x20000000` (reset PC13: the active-low LED lights) and `0x00002000` (set: it goes dark).
It also checks the text and exit status of the other five programs. `checks` compares against known values
(`fib(90)`, `20!`, and the standard CRC-32 check value `0xCBF43926` for `"123456789"`).

## What has not been tested

Nothing here has run on a physical board. The emulated machine is the STM32VLDISCOVERY (STM32F100, Cortex-M3);
the blinky uses the register layout and addresses that the STM32F103 "Blue Pill" shares with it, but its clock
setup is simply the chip's power-on state (8 MHz internal oscillator), and on real silicon only the LED
timing and the active-low polarity of the Blue Pill's LED are things I could not check. To try it, build
`build/blinky-hw.bin` (`make build/blinky-hw.bin`) and write it to `0x08000000` with your usual tool, for
example `st-flash write build/blinky-hw.bin 0x8000000`.

## Notes for your own board code

- The generated C needs no C library. The compiler may still emit calls to `memcpy`, `memset` or `memmove` for
  large copies and zero-initialisation, and a 32-bit CPU needs the 64-bit helpers from libgcc
  (`__aeabi_uldivmod` and friends); link `-lgcc` and a libc such as newlib-nano, as `Makefile` does.
- Define `noge_trap_hook(int kind, int line)` to decide what a failed check does. Without one, the weak default
  in the generated code loops forever. Kinds: 1 overflow, 2 division by zero, 3 index out of bounds, 4 shift.
- C structs passed to or from Noge must have the same field types in the same order as the Noge `struct`.
- CMSIS-style helpers that are macros or inline assembly (`__disable_irq()`, `__WFI()`) cannot be called
  directly: wrap them in a small C function and declare that with `extern "C"`.
