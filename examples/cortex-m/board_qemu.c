/* Board support for running under QEMU (-M stm32vldiscovery -semihosting): output and exit go through
   ARM semihosting, so no UART setup is needed. Noge programs call these through extern "C". */
#include <stdint.h>

static int semihost(int op, const void *arg) {
    register int r0 __asm__("r0") = op;
    register const void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void semihost_puts(const uint8_t *s) { semihost(0x04, s); } /* SYS_WRITE0: print a NUL-terminated string */

void semihost_putbuf(const uint8_t *s) { semihost(0x04, s); } /* the same, for an array of bytes */

void semihost_exit(int code) __attribute__((noreturn));
void semihost_exit(int code) { /* SYS_EXIT_EXTENDED: QEMU exits with this status */
    uint32_t block[2] = {0x20026, (uint32_t)code};
    semihost(0x20, block);
    for (;;) {
    }
}

/* the blinky calls this once per half period; after six, the test is over */
void board_tick(void) {
    static int ticks;
    if (++ticks == 6) {
        semihost_puts((const uint8_t *)"blinky: six half-periods\n");
        semihost_exit(0);
    }
}

/* a counter owned by C and shared with an interrupt handler written in Noge */
static volatile uint32_t g_ticks;
volatile uint32_t *board_ticks(void) { return &g_ticks; }

/* a failed run-time check in a Noge program ends up here */
static void put_uint(uint32_t v) {
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[13];
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    out[n] = 0;
    semihost(0x04, out);
}

void noge_trap_hook(int kind, int line) __attribute__((noreturn));
void noge_trap_hook(int kind, int line) {
    semihost(0x04, "TRAP kind=");
    put_uint((uint32_t)kind);
    semihost(0x04, " line=");
    put_uint((uint32_t)line);
    semihost(0x04, "\n");
    semihost_exit(99);
}

void HardFault_Handler(void) {
    semihost(0x04, "HardFault\n");
    semihost_exit(98);
}
