#include "clock.h"
#include "nrf.h"
#include <assert.h>
#include <stdio.h>

test_timer_t test_timer0, test_timer1;
test_clock_t test_clock;
unsigned test_ipsr, test_irq_enabled, test_irq_pending, test_irq_priority;
void TIMER1_IRQHandler(void);
void NVIC_DisableIRQ(unsigned irq) { assert(irq == TIMER1_IRQn); test_irq_enabled = 0; }
void NVIC_EnableIRQ(unsigned irq) { assert(irq == TIMER1_IRQn); test_irq_enabled = 1; }
void NVIC_ClearPendingIRQ(unsigned irq) { assert(irq == TIMER1_IRQn); test_irq_pending = 0; }
void NVIC_SetPriority(unsigned irq, unsigned priority)
{ assert(irq == TIMER1_IRQn); test_irq_priority = priority; }

/* No CPU/SysTick, USB or RADIO activity while asleep. Only a running peripheral
 * timer can produce the interrupt needed for the first foreground USB poll.
 * Return zero for no wakeup within 10 ms; never hang a test on the old bug. */
static uint32_t sleep_until_interrupt(void)
{
    static bool running;
    static uint32_t counter;
    if (NRF_TIMER1->TASKS_STOP) { running = false; NRF_TIMER1->TASKS_STOP = 0; }
    if (NRF_TIMER1->TASKS_CLEAR) { counter = 0; NRF_TIMER1->TASKS_CLEAR = 0; }
    if (NRF_TIMER1->TASKS_START) { running = true; NRF_TIMER1->TASKS_START = 0; }
    assert(NRF_TIMER1->MODE == TIMER_MODE_MODE_Timer);
    for (uint32_t us = 1; us <= 10000; ++us) {
        ++NRF_TIMER0->CC[0];
        if (!running) continue;
        counter += 16u >> NRF_TIMER1->PRESCALER;
        if (counter == NRF_TIMER1->CC[0]) {
            NRF_TIMER1->EVENTS_COMPARE[0] = 1;
            if (NRF_TIMER1->SHORTS & TIMER_SHORTS_COMPARE0_CLEAR_Msk) counter = 0;
            if (test_irq_enabled && (NRF_TIMER1->INTENSET & TIMER_INTENSET_COMPARE0_Msk))
                return us;
        }
    }
    return 0;
}
int main(void)
{
    test_irq_pending = 1;
    NRF_TIMER1->EVENTS_COMPARE[0] = 1; /* Stale boot state must be cleared. */
    time_init();
    assert(!test_irq_pending && !NRF_TIMER1->EVENTS_COMPARE[0]);
    assert(test_irq_priority > 6); /* RADIO and USB preempt housekeeping. */
    uint32_t before = time_us();
    for (unsigned i = 0; i < 3; ++i) {
        assert(sleep_until_interrupt() == 1000);
        assert(NRF_TIMER1->EVENTS_COMPARE[0]);
        TIMER1_IRQHandler();
        assert(!NRF_TIMER1->EVENTS_COMPARE[0]);
    }
    assert(time_us() - before == 3000); /* USB poll/timeout timebase progressed. */
    time_wakeup_stop();
    assert(!test_irq_enabled && !test_irq_pending);
    assert(sleep_until_interrupt() == 0);
    puts("Clock: idle wakeup without CPU/SysTick, periodic IRQ acknowledgement, stop passed");
}
