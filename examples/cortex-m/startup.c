/* Start-up code for a Cortex-M3: the vector table, and the reset handler that sets up RAM and calls main.
   Any handler a program defines (for example SysTick_Handler in Noge, with export "C") replaces the weak
   default below. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void Reset_Handler(void);

void Default_Handler(void) {
    for (;;) {
    }
}

#define WEAK_HANDLER(name) void name(void) __attribute__((weak, alias("Default_Handler")))
WEAK_HANDLER(NMI_Handler);
WEAK_HANDLER(HardFault_Handler);
WEAK_HANDLER(MemManage_Handler);
WEAK_HANDLER(BusFault_Handler);
WEAK_HANDLER(UsageFault_Handler);
WEAK_HANDLER(SVC_Handler);
WEAK_HANDLER(DebugMon_Handler);
WEAK_HANDLER(PendSV_Handler);
WEAK_HANDLER(SysTick_Handler);

__attribute__((section(".isr_vector"), used)) void (*const g_vectors[])(void) = {
    (void (*)(void))&_estack, /* initial stack pointer */
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
};

void Reset_Handler(void) {
    for (uint32_t *src = &_sidata, *dst = &_sdata; dst < &_edata;) *dst++ = *src++; /* initialized data */
    for (uint32_t *dst = &_sbss; dst < &_ebss;) *dst++ = 0;                           /* zeroed data */
    main();
    for (;;) {
    }
}
