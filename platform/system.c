#include "system.h"
#include "nrf.h"

/* Vendor SystemInit supplies revision-gated silicon errata and FPU setup.
 * These options would permit its optional UICR/GPIO writes; forbid them. */
#if defined(CONFIG_NFCT_PINS_AS_GPIOS) || defined(CONFIG_GPIO_AS_PINRESET) || \
    defined(ENABLE_SWO) || defined(ENABLE_TRACE) || defined(SOFTDEVICE_PRESENT)
#error "This application must not reconfigure UICR, camera pins, or SoftDevice"
#endif
#include "system_nrf52840.c"

extern const uintptr_t vectors[];
volatile uint32_t platform_fault_code;

void platform_init(void)
{
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    for (unsigned i = 0; i < 2; ++i) {
        NVIC->ICER[i] = 0xffffffffu;
        NVIC->ICPR[i] = 0xffffffffu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR = (uint32_t)(uintptr_t)vectors;
    __DSB();
    __ISB();
    SystemInit();
    /* No PPI routes inherited from a prior bootloader peripheral setup. */
    NRF_PPI->CHENCLR = 0xffffffffu;
    NRF_CLOCK->INTENCLR = 0xffffffffu;
    NRF_POWER->INTENCLR = 0xffffffffu;
    __set_BASEPRI(0);
    __set_FAULTMASK(0);
    __enable_irq();
}

void platform_fault(uint32_t code)
{
    __disable_irq();
    platform_fault_code = code; /* Inspect with SWD; no GPIO/camera activity. */
    NRF_RADIO->POWER = 0;
    for (;;) __WFE();
}
