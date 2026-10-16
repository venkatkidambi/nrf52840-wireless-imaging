#include "radio_timing.h"
#include "nrf.h"

radio_timing_stats_t radio_timing_stats;
static uint64_t extended_time;
static uint32_t previous_time;
static void ppi(unsigned ch, volatile uint32_t *event, volatile uint32_t *task,
                volatile uint32_t *fork)
{
    NRF_PPI->CH[ch].EEP = (uint32_t)(uintptr_t)event;
    NRF_PPI->CH[ch].TEP = (uint32_t)(uintptr_t)task;
    NRF_PPI->FORK[ch].TEP = (uint32_t)(uintptr_t)fork;
}
void radio_timing_init(void)
{
    /* TIMER3: END=CC0, ADDRESS=CC1, CPU=CC2, START=CC3, deadline=CC4.
     * PPI 0..4, group 0, EGU0 event 0. No shared USB resources. */
    extended_time = 0; previous_time = 0;
    NRF_PPI->CHENCLR = 31u;
    NRF_TIMER3->TASKS_STOP = 1;
    NRF_TIMER3->INTENCLR = 0xffffffffu;
    NRF_TIMER3->SHORTS = 0;
    NRF_TIMER3->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER3->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER3->PRESCALER = 4;
    NRF_TIMER3->TASKS_CLEAR = 1;
    NRF_TIMER3->EVENTS_COMPARE[4] = 0;
    NRF_TIMER3->TASKS_START = 1;
    NRF_EGU0->INTENCLR = 0xffffffffu;
    ppi(0, &NRF_RADIO->EVENTS_END, &NRF_TIMER3->TASKS_CAPTURE[0], 0);
    ppi(1, &NRF_RADIO->EVENTS_ADDRESS, &NRF_TIMER3->TASKS_CAPTURE[1], 0);
    ppi(2, &NRF_EGU0->EVENTS_TRIGGERED[0], &NRF_RADIO->TASKS_START,
        &NRF_TIMER3->TASKS_CAPTURE[3]);
    ppi(3, &NRF_TIMER3->EVENTS_COMPARE[4], &NRF_RADIO->TASKS_START,
        &NRF_TIMER3->TASKS_CAPTURE[3]);
    ppi(4, &NRF_TIMER3->EVENTS_COMPARE[4], &NRF_PPI->TASKS_CHG[0].DIS, 0);
    NRF_PPI->CHG[0] = (1u << 3) | (1u << 4);
    NRF_PPI->CHENSET = 7u;
}
uint32_t radio_time(void)
{
    NRF_TIMER3->TASKS_CAPTURE[2] = 1;
    return NRF_TIMER3->CC[2];
}
uint64_t radio_time64(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t now = radio_time();
    extended_time += (uint32_t)(now - previous_time);
    previous_time = now;
    uint64_t value = extended_time;
    __set_PRIMASK(mask);
    return value;
}
uint32_t radio_end_time(void) { return NRF_TIMER3->CC[0]; }
uint32_t radio_address_time(void) { return NRF_TIMER3->CC[1]; }
uint32_t radio_start_time(void) { return NRF_TIMER3->CC[3]; }
void radio_timing_cancel(void) { NRF_PPI->TASKS_CHG[0].DIS = 1; }
void radio_start_now(void)
{
    NRF_EGU0->EVENTS_TRIGGERED[0] = 0;
    NRF_EGU0->TASKS_TRIGGER[0] = 1;
    __DSB();
}
bool radio_start_at(uint32_t deadline)
{
    radio_timing_cancel();
    NRF_TIMER3->EVENTS_COMPARE[4] = 0;
    NRF_TIMER3->CC[4] = deadline;
    /* Caller masks interrupts during this bounded arm transaction. Eight us
     * covers register writes; a missed compare must never wait for timer wrap. */
    bool on_time = measurement_deadline_safe(radio_time(), deadline);
    if (!on_time) {
        /* Rebase late rather than starting early or waiting for timer wrap. */
        NRF_TIMER3->CC[4] = radio_time() + 8u;
        NRF_TIMER3->EVENTS_COMPARE[4] = 0;
    }
    NRF_PPI->TASKS_CHG[0].EN = 1;
    return on_time;
}
void radio_timing_rearm(uint32_t end, uint32_t handled)
{
    uint32_t start = radio_start_time();
    if ((uint32_t)(start - end) >= 1000000u) {
        ++radio_timing_stats.rejected;
        return;
    }
    measurement_add(&radio_timing_stats.rearm, start - end);
    measurement_add(&radio_timing_stats.poll, handled - end);
    measurement_add(&radio_timing_stats.software, start - handled);
    /* RXIDLE -> START has no RXEN/READY ramp. */
    measurement_add(&radio_timing_stats.ramp, 0);
}
