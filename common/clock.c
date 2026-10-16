#include "clock.h"
#include "nrf.h"

void time_init(void)
{
    NRF_TIMER0->TASKS_STOP = 1;
    NRF_TIMER0->INTENCLR = 0xffffffffu;
    NRF_TIMER0->SHORTS = 0;
    NRF_TIMER0->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER0->PRESCALER = 4; /* 16 MHz / 16 = 1 MHz; works before HFXO. */
    NRF_TIMER0->TASKS_CLEAR = 1;
    NRF_TIMER0->TASKS_START = 1;

    /* nRF52 gates SysTick with the CPU during WFI/WFE. TIMER1 runs from
     * peripheral HFCLK and wakes idle RX before USB has any interrupt source.
     * Its periodic compare also bounds TX waits if RADIO fails to raise END. */
    NVIC_DisableIRQ(TIMER1_IRQn);
    NRF_TIMER1->TASKS_STOP = 1;
    NRF_TIMER1->INTENCLR = 0xffffffffu;
    NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER1->PRESCALER = 4;
    NRF_TIMER1->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
    NRF_TIMER1->CC[0] = 1000u;
    NRF_TIMER1->EVENTS_COMPARE[0] = 0;
    NRF_TIMER1->TASKS_CLEAR = 1;
    NVIC_ClearPendingIRQ(TIMER1_IRQn);
    NVIC_SetPriority(TIMER1_IRQn, 7);
    NRF_TIMER1->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
    NVIC_EnableIRQ(TIMER1_IRQn);
    NRF_TIMER1->TASKS_START = 1;
}

void TIMER1_IRQHandler(void)
{
    NRF_TIMER1->EVENTS_COMPARE[0] = 0;
    __DSB();
}

void time_wakeup_stop(void)
{
    NVIC_DisableIRQ(TIMER1_IRQn);
    NRF_TIMER1->INTENCLR = 0xffffffffu;
    NRF_TIMER1->TASKS_STOP = 1;
    NRF_TIMER1->EVENTS_COMPARE[0] = 0;
    NVIC_ClearPendingIRQ(TIMER1_IRQn);
}

uint32_t time_us(void)
{
    /* USB startup waits also use this function in the ISR. Separate captures
     * prevent an interrupt from overwriting a foreground timestamp. */
    unsigned channel = __get_IPSR() == (16u + RADIO_IRQn) ? 2u :
                       (__get_IPSR() ? 1u : 0u);
    NRF_TIMER0->TASKS_CAPTURE[channel] = 1;
    return NRF_TIMER0->CC[channel];
}

bool wait_event(volatile uint32_t *event, uint32_t timeout_us)
{
    uint32_t start = time_us();
    while (!*event)
        if ((uint32_t)(time_us() - start) >= timeout_us) return false;
    return true;
}

bool clock_hf_running(void)
{
    return (NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_STATE_Msk | CLOCK_HFCLKSTAT_SRC_Msk)) ==
           ((CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos) |
            (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos));
}

bool clock_init(void)
{
    if (clock_hf_running()) return true;
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    return wait_event(&NRF_CLOCK->EVENTS_HFCLKSTARTED, 100000u) && clock_hf_running();
}
