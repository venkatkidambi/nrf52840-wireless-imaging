#include "radio.h"
#include "radio_timing.h"
#include "nrf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
fake_radio_t fake_radio;
uint32_t fake_mask;
static uint32_t tick, start, address, end, starts;
static bool stall, fail_event;
static _Alignas(4) uint8_t buffers[3][RADIO_PACKET_SIZE];
void RADIO_IRQHandler(void);
void NVIC_DisableIRQ(unsigned irq) { (void)irq; }
void NVIC_EnableIRQ(unsigned irq) { (void)irq; }
void NVIC_ClearPendingIRQ(unsigned irq) { (void)irq; }
void NVIC_SetPriority(unsigned irq, unsigned priority) { (void)irq; (void)priority; }
bool clock_hf_running(void) { return true; }
uint32_t time_us(void) { return tick; }
uint32_t radio_time(void) { return tick; }
uint32_t radio_start_time(void) { return start; }
uint32_t radio_address_time(void) { return address; }
uint32_t radio_end_time(void) { return end; }
void radio_timing_cancel(void) {}
void radio_timing_rearm(uint32_t a, uint32_t b) { (void)a; (void)b; }
bool wait_event(volatile uint32_t *event, uint32_t timeout)
{
    (void)timeout;
    if (fail_event) return false;
    *event = 1;
    if (event == &fake_radio.EVENTS_DISABLED) fake_radio.STATE = RADIO_STATE_STATE_Disabled;
    else {
        fake_radio.STATE = fake_radio.TASKS_RXEN ? RADIO_STATE_STATE_RxIdle : RADIO_STATE_STATE_TxIdle;
        fake_radio.TASKS_RXEN = fake_radio.TASKS_TXEN = 0;
    }
    return true;
}
void radio_start_now(void)
{
    assert(fake_radio.STATE == RADIO_STATE_STATE_TxIdle || fake_radio.STATE == RADIO_STATE_STATE_RxIdle);
    start = tick; address = start + 24; ++starts;
    fake_radio.STATE = fake_radio.STATE == RADIO_STATE_STATE_TxIdle ? RADIO_STATE_STATE_Tx : RADIO_STATE_STATE_Rx;
}
bool radio_start_at(uint32_t deadline)
{
    bool safe = measurement_deadline_safe(tick, deadline);
    tick = safe ? deadline : tick + 8;
    radio_start_now(); return safe;
}
static void finish(void)
{
    assert(fake_radio.STATE == RADIO_STATE_STATE_Tx);
    end = tick = start + RADIO_AIRTIME_US;
    fake_radio.STATE = RADIO_STATE_STATE_TxIdle;
    fake_radio.EVENTS_END = fake_radio.EVENTS_ADDRESS = 1;
    RADIO_IRQHandler();
}
void __WFI(void) { if (stall) tick += 1000; else finish(); }
static void reset(uint32_t t)
{
    memset(&fake_radio, 0, sizeof fake_radio);
    tick = t; starts = 0; stall = fail_event = false; fake_mask = 0;
    assert(radio_init() && radio_tx_enable());
}
static const uint8_t *received_buffers[2];
static unsigned received;
static void receive(const uint8_t *packet, bool crc, uint32_t ended)
{
    assert(crc && ended == end && received < 2);
    assert(fake_radio.STATE == RADIO_STATE_STATE_Rx); /* Rearm BEFORE callback. */
    assert((uintptr_t)packet % 4 == 0);
    received_buffers[received++] = packet;
}
int main(void)
{
    radio_tx_result_t a, b;
    const uint32_t intervals[] = {TX_MIN_INTERVAL_US, 1100, 1200, 1500, 0};
    for (unsigned i = 0; i < sizeof intervals / sizeof intervals[0]; ++i) {
        reset(UINT32_MAX - 500);
        assert(radio_tx_submit(buffers[0], false, 0));
        assert(!radio_tx_submit(buffers[2], false, 0));
        assert(!radio_tx_stage(buffers[1], TX_MIN_INTERVAL_US - 1));
        assert(!radio_tx_stage(buffers[1], 60000001));
        assert(radio_tx_stage(buffers[1], intervals[i]));
        assert(!radio_tx_stage(buffers[2], intervals[i]));
        finish(); /* ISR starts alternate buffer before foreground consumes. */
        finish(); /* Both completions may accumulate without overwriting either. */
        assert(radio_tx_wait(&a) && radio_tx_wait(&b));
        assert(starts == 2 && b.on_time && b.start - a.start == (intervals[i] ? intervals[i] : TX_MIN_INTERVAL_US));
        assert(b.scheduled == (intervals[i] != 0));
        assert(!radio_tx_wait(&b));
    }
    reset(0);
    assert(radio_tx_submit(buffers[0], false, 0)); finish(); tick += 100;
    assert(radio_tx_stage(buffers[1], TX_MIN_INTERVAL_US));
    assert(radio_tx_wait(&a) && radio_tx_wait(&b));
    assert(!b.on_time && b.start == a.end + 108); /* One late arm, no burst. */
    reset(0);
    assert(radio_tx_submit(buffers[0], false, 0));
    assert(radio_tx_stage(buffers[1], 1500));
    assert(radio_disable()); assert(!radio_tx_wait(&a));
    assert(starts == 1); /* Cancellation drops staged work, stops DMA. */
    reset(0); stall = true;
    assert(radio_tx_submit(buffers[0], false, 0));
    assert(!radio_tx_wait(&a) && fake_radio.STATE == RADIO_STATE_STATE_Disabled);
    reset(0); fail_event = true;
    assert(!radio_disable() && fake_radio.POWER == 0);
    reset(0);
    assert(radio_receive(receive) && radio_rx_healthy());
    assert(!radio_tx_submit(buffers[0], false, 0));
    assert(!radio_tx_stage(buffers[0], 1500));
    for (unsigned i = 0; i < 2; ++i) {
        fake_radio.STATE = RADIO_STATE_STATE_RxIdle;
        fake_radio.EVENTS_END = fake_radio.EVENTS_ADDRESS = fake_radio.CRCSTATUS = 1;
        end = tick += RADIO_AIRTIME_US;
        RADIO_IRQHandler();
    }
    assert(received == 2 && received_buffers[0] != received_buffers[1]);
    assert((uintptr_t)received_buffers[1] - (uintptr_t)received_buffers[0] == ((RADIO_PACKET_SIZE + 3) & ~3u));
    fake_radio.EVENTS_ADDRESS = 1; tick = address + 2001;
    assert(!radio_rx_healthy());
    assert(radio_tx_enable() && fake_radio.STATE == RADIO_STATE_STATE_TxIdle);
    puts("RADIO: staged ownership, ordered completions, wrap, admission, late rebase, cancellation, bounded faults passed");
}
