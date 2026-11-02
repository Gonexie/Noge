/* Board support for real hardware: nothing to report to, so the hooks do nothing. The default run-time
   check handler (noge_trap_hook, weak in the generated code) halts the chip. */
#include <stdint.h>

void board_tick(void) {}
