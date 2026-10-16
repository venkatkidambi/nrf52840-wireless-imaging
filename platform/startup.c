#include <stdint.h>
#include "nrf.h"
#include "system.h"
extern uint32_t __stack_top, __data_start, __data_end, __data_load;
extern uint32_t __bss_start, __bss_end;
int main(void);
void Reset_Handler(void);
void Default_Handler(void) { platform_fault(0x100u + __get_IPSR()); }
void RADIO_IRQHandler(void);
void TIMER1_IRQHandler(void);
void USBD_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

/* nRF52840 has IRQs 0..47. All unused/reserved slots have a trap handler. */
__attribute__((section(".vectors"), used, aligned(256)))
const uintptr_t vectors[64] = {
    [0] = (uintptr_t)&__stack_top,
    [1] = (uintptr_t)Reset_Handler,
    [2 ... 16] = (uintptr_t)Default_Handler,
    [17] = (uintptr_t)RADIO_IRQHandler,
    [18 ... 24] = (uintptr_t)Default_Handler,
    [25] = (uintptr_t)TIMER1_IRQHandler, /* 16 + TIMER1_IRQn (9) */
    [26 ... 54] = (uintptr_t)Default_Handler,
    [55] = (uintptr_t)USBD_IRQHandler, /* 16 + USBD_IRQn (39) */
    [56 ... 63] = (uintptr_t)Default_Handler
};
_Static_assert(USBD_IRQn == 39, "Unexpected nRF52840 IRQ definition");
_Static_assert(TIMER1_IRQn == 9, "Unexpected wakeup timer IRQ definition");

void Reset_Handler(void)
{
    __disable_irq();
    uint32_t *src = &__data_load;
    for (uint32_t *p = &__data_start; p < &__data_end;) *p++ = *src++;
    for (uint32_t *p = &__bss_start; p < &__bss_end;) *p++ = 0;
    platform_init();
    (void)main();
    platform_fault(1);
}
