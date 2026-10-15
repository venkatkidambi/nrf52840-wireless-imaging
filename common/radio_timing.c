#include "radio_timing.h"
#include "nrf.h"

radio_timing_stats_t radio_timing_stats;
static bool enabled, pending;
static uint32_t end_stamp, poll_stamp;

void radio_timing_init(void)
{
    /* RX owns TIMER2, EGU0 event 0 and programmable PPI channels 0..2.
     * No IRQs. TIMER0 remains the system timebase. 1 tick = 1 microsecond. */
    NRF_TIMER2->TASKS_STOP = 1;
    NRF_TIMER2->INTENCLR = 0xffffffffu;
    NRF_TIMER2->SHORTS = 0;
    NRF_TIMER2->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER2->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER2->PRESCALER = 4;
    NRF_TIMER2->TASKS_CLEAR = 1;
    NRF_TIMER2->TASKS_START = 1;
    NRF_EGU0->INTENCLR = 0xffffffffu;
    NRF_PPI->CHENCLR = 7u;
    NRF_PPI->CH[0].EEP = (uint32_t)(uintptr_t)&NRF_RADIO->EVENTS_END;
    NRF_PPI->CH[0].TEP = (uint32_t)(uintptr_t)&NRF_TIMER2->TASKS_CAPTURE[0];
    NRF_PPI->FORK[0].TEP = 0;
    NRF_PPI->CH[1].EEP = (uint32_t)(uintptr_t)&NRF_RADIO->EVENTS_READY;
    NRF_PPI->CH[1].TEP = (uint32_t)(uintptr_t)&NRF_TIMER2->TASKS_CAPTURE[1];
    NRF_PPI->FORK[1].TEP = 0;
    /* One hardware event issues RXEN and captures its timestamp together.
     * This avoids attributing an interrupt before a CPU RXEN write to ramp-up. */
    NRF_PPI->CH[2].EEP = (uint32_t)(uintptr_t)&NRF_EGU0->EVENTS_TRIGGERED[0];
    NRF_PPI->CH[2].TEP = (uint32_t)(uintptr_t)&NRF_RADIO->TASKS_RXEN;
    NRF_PPI->FORK[2].TEP = (uint32_t)(uintptr_t)&NRF_TIMER2->TASKS_CAPTURE[3];
    NRF_PPI->CHENSET = 7u;
    enabled = true;
}

void radio_timing_end(void)
{
    if (!enabled) return;
    NRF_TIMER2->TASKS_CAPTURE[2] = 1;
    poll_stamp = NRF_TIMER2->CC[2];
    end_stamp = NRF_TIMER2->CC[0];
    pending = true;
}

void radio_timing_rxen(void)
{
    if (enabled) {
        NRF_EGU0->EVENTS_TRIGGERED[0] = 0;
        NRF_EGU0->TASKS_TRIGGER[0] = 1;
    } else NRF_RADIO->TASKS_RXEN = 1;
}

void radio_timing_ready(void)
{
    if (!pending) return; /* Initial arm/recovery is not a packet turnaround. */
    uint32_t spans[4];
    if (measurement_spans(end_stamp, poll_stamp, NRF_TIMER2->CC[3],
                          NRF_TIMER2->CC[1], spans)) {
        measurement_add(&radio_timing_stats.rearm, spans[0]);
        measurement_add(&radio_timing_stats.poll, spans[1]);
        measurement_add(&radio_timing_stats.software, spans[2]);
        measurement_add(&radio_timing_stats.ramp, spans[3]);
    } else ++radio_timing_stats.rejected;
    pending = false;
}

void radio_timing_cancel(void)
{
    if (pending) ++radio_timing_stats.rejected;
    pending = false;
}
