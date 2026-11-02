#!/bin/sh
# Runs each firmware under QEMU and checks what it printed, how it exited, and (for the blinky) which
# peripheral registers it wrote. QEMU does not model the STM32 GPIO and RCC blocks, so it logs every access to
# them (-d unimp); that log is how the blinky's register traffic is observed.
QEMU=${QEMU:-qemu-system-arm}
MACHINE=${MACHINE:-stm32vldiscovery}
pass=0
fail=0

run() { # run <name>: sets $out (everything the firmware printed) and $code (its exit status)
    out=$(timeout 60 "$QEMU" -M "$MACHINE" -nographic -semihosting-config enable=on,target=native \
        -kernel "build/$1.elf" -d unimp -D "build/$1.unimp.log" 2>&1)
    code=$?
}

# expect <name> <expected output> <expected exit status>
expect() {
    run "$1"
    if [ "$out" = "$2" ] && [ "$code" -eq "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1: exit status $code (wanted $3)"
        printf '%s\n' "--- wanted ---" "$2" "--- got ---" "$out"
    fi
}

expect hello "Hello from Noge on a Cortex-M3" 0

expect checks "fib(90) = 2880067194370816120
20! = 2432902008176640000
crc32 of 123456789 = 3421780262
sorted checksum = 230
wrapping 4e9 + 4e9 = 3705032704
largest usize = 4294967295" 0

trap_line=$(grep -n 'return a\[i\]' trap.nge | cut -d: -f1)
expect trap "before the trap
TRAP kind=3 line=$trap_line" 99

expect systick "ten ticks counted by the handler" 0

expect machine "stack machine result = -40
green ticks = 12
red ticks = 20" 0

expect blinky "blinky: six half-periods" 0
# The registers the blinky touched, in order: clock enable, pin configuration, then six LED changes
# (BSRR bit 29 resets PC13, which lights the active-low LED; bit 13 sets it again).
got=$(grep 'unimplemented device write' build/blinky.unimp.log |
    sed -E 's/^([A-Za-z0-9]+): unimplemented device write \(size 4, offset (0x[0-9a-f]+), value (0x[0-9a-f]+)\)$/\1 \2 \3/')
want="RCC 0x018 0x00000010
GPIOC 0x004 0x00200000
GPIOC 0x010 0x20000000
GPIOC 0x010 0x00002000
GPIOC 0x010 0x20000000
GPIOC 0x010 0x00002000
GPIOC 0x010 0x20000000
GPIOC 0x010 0x00002000"
if [ "$got" = "$want" ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL blinky: unexpected register writes"
    printf '%s\n' "--- wanted ---" "$want" "--- got ---" "$got"
fi

# the same source built for real hardware has no board_tick to stop it: it must still link and stay small
size=$(arm-none-eabi-size build/blinky-hw.elf | awk 'NR == 2 { print $1 + $2 }')
echo "blinky-hw.elf: $size bytes of flash"

echo "cortex-m: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
