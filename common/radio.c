#include "radio.h"
#include "clock.h"
#include "radio_timing.h"
#include "nrf.h"

/* Each array element is independently aligned, including the 255-byte case. */
static struct { uint8_t bytes[RADIO_PACKET_SIZE]; } __attribute__((aligned(4))) dma[2];
static unsigned active;
static radio_rx_handler_t rx_handler;
static radio_tx_result_t tx_results[2], tx_previous;
static volatile uint32_t tx_produced, tx_consumed, tx_outstanding;
static volatile bool tx_active;
static bool tx_scheduled, tx_on_time;
static uint32_t tx_deadline, staged_interval;
static const uint8_t * volatile staged_packet;
static volatile uint32_t tx_wait_bound;
volatile uint32_t radio_rx_events;

bool radio_disable(void)
{
    NVIC_DisableIRQ(RADIO_IRQn);
    NRF_RADIO->INTENCLR = 0xffffffffu;
    radio_timing_cancel();
    tx_active = false; staged_packet = 0;
    tx_produced = tx_consumed = tx_outstanding = 0;
    rx_handler = 0;
    NRF_RADIO->SHORTS = 0;
    if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) return true;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    if (wait_event(&NRF_RADIO->EVENTS_DISABLED, 2000u)) return true;
    NRF_RADIO->POWER = 0; /* Stop DMA even on timeout. */
    return false;
}
bool radio_init(void)
{
    if (!radio_disable() || !clock_hf_running()) return false;
    NRF_RADIO->POWER = 0;
    __DSB();
    NRF_RADIO->POWER = 1;
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;
    NRF_RADIO->FREQUENCY =
        ((RADIO_FREQ_MAP_LOW ? RADIO_FREQUENCY_MAP_Low : RADIO_FREQUENCY_MAP_Default)
         << RADIO_FREQUENCY_MAP_Pos) |
        (RADIO_FREQ_OFFSET << RADIO_FREQUENCY_FREQUENCY_Pos);
    NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_0dBm;
    /* No S0, S1, or LENGTH fields in SRAM/on air. 8-bit proprietary preamble. */
    NRF_RADIO->PCNF0 = RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos;
    NRF_RADIO->PCNF1 = (RADIO_PAYLOAD_SIZE << RADIO_PCNF1_MAXLEN_Pos) |
        (RADIO_PAYLOAD_SIZE << RADIO_PCNF1_STATLEN_Pos) |
        (4u << RADIO_PCNF1_BALEN_Pos) |
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
        (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);
    NRF_RADIO->BASE0 = 0x89abcdefu;
    NRF_RADIO->PREFIX0 = 0xc3;
    NRF_RADIO->TXADDRESS = 0;
    NRF_RADIO->RXADDRESSES = 1;
    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = 0x11021;
    NRF_RADIO->CRCINIT = 0xffff;
    NRF_RADIO->SHORTS = 0;
    return true;
}
static void clear_packet_events(void)
{
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;
    NRF_RADIO->EVENTS_CRCERROR = 0;
}
static bool enable(bool rx)
{
    if (!radio_disable()) return false;
    clear_packet_events();
    NRF_RADIO->EVENTS_READY = 0;
    if (rx) NRF_RADIO->TASKS_RXEN = 1;
    else NRF_RADIO->TASKS_TXEN = 1;
    if (!wait_event(&NRF_RADIO->EVENTS_READY, 2000u)) {
        (void)radio_disable();
        return false;
    }
    NVIC_ClearPendingIRQ(RADIO_IRQn);
    NVIC_SetPriority(RADIO_IRQn, 1);
    NRF_RADIO->INTENSET = RADIO_INTENSET_END_Msk;
    NVIC_EnableIRQ(RADIO_IRQn);
    return true;
}
bool radio_receive(radio_rx_handler_t handler)
{
    if (!enable(true)) return false;
    active = 0;
    rx_handler = handler;
    NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)dma[active].bytes;
    __DMB();
    radio_start_now();
    return true;
}
bool radio_tx_enable(void) { return enable(false); }
/* IRQs masked, or RADIO IRQ (highest enabled priority); RADIO is TXIDLE.
 * Publish DMA before arming the one-shot PPI group. */
static bool tx_start(const uint8_t *packet, bool scheduled, uint32_t deadline)
{
    clear_packet_events();
    NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)packet;
    __DMB();
    int32_t remaining = scheduled ? (int32_t)(deadline - radio_time()) : 0;
    tx_wait_bound = (remaining > 0 ? (uint32_t)remaining : 0) + 3000u;
    tx_active = true; tx_scheduled = scheduled; tx_deadline = deadline;
    tx_on_time = true;
    if (scheduled) tx_on_time = radio_start_at(deadline);
    else radio_start_now();
    return tx_on_time;
}
bool radio_tx_submit(const uint8_t *packet, bool scheduled, uint32_t deadline)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (tx_outstanding || NRF_RADIO->STATE != RADIO_STATE_STATE_TxIdle) {
        __set_PRIMASK(mask); return false;
    }
    tx_outstanding = 1;
    bool on_time = tx_start(packet, scheduled, deadline);
    __set_PRIMASK(mask);
    return on_time;
}
static void tx_start_staged(void)
{
    const uint8_t *packet = staged_packet;
    staged_packet = 0;
    uint32_t deadline = staged_interval ? tx_previous.start + staged_interval : tx_previous.end + 20u;
    (void)tx_start(packet, true, deadline);
    tx_scheduled = staged_interval != 0; /* Max-mode backoff is not a missed fixed deadline. */
}
bool radio_tx_stage(const uint8_t *packet, uint32_t interval)
{
    if (interval && (interval < TX_MIN_INTERVAL_US || interval > 60000000u)) return false;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (tx_outstanding != 1 || staged_packet || rx_handler ||
        (!tx_active && NRF_RADIO->STATE != RADIO_STATE_STATE_TxIdle)) {
        __set_PRIMASK(mask); return false;
    }
    ++tx_outstanding; staged_packet = packet; staged_interval = interval;
    __DMB();
    if (!tx_active) tx_start_staged(); /* Producer arrived after END. */
    __set_PRIMASK(mask);
    return true;
}
void RADIO_IRQHandler(void)
{
    if (!NRF_RADIO->EVENTS_END) return;
    uint32_t end = radio_end_time();
    if (rx_handler) {
        uint32_t handled = radio_time();
        bool crc = NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk;
        unsigned finished = active;
        /* END: CRC and DMA complete, RXIDLE. Snapshot status BEFORE START.
         * No END_START: even a delayed IRQ cannot reuse an unread buffer. */
        clear_packet_events();
        active ^= 1u;
        NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)dma[active].bytes;
        __DMB();
        radio_start_now();
        ++radio_rx_events;
        radio_timing_rearm(end, handled);
        rx_handler(dma[finished].bytes, crc, end);
        /* If another END arrives during validation it waits in RXIDLE until
         * this non-reentrant ISR returns; no overwrite and no hidden queue. */
    } else if (tx_active) {
        radio_tx_result_t result = {
            .start = radio_start_time(), .address = radio_address_time(), .end = end,
            .deadline = tx_deadline, .addressed = NRF_RADIO->EVENTS_ADDRESS != 0,
            .scheduled = tx_scheduled, .on_time = tx_on_time};
        NRF_RADIO->EVENTS_END = 0;
        tx_active = false;
        tx_previous = result;
        /* Start the alternate buffer before foreground telemetry/bookkeeping.
         * No END_START shortcut: an absent producer must leave RADIO idle. */
        if (staged_packet) tx_start_staged();
        tx_results[tx_produced & 1u] = result;
        __DMB();
        ++tx_produced;
    }
    __DSB();
}
bool radio_tx_wait(radio_tx_result_t *result)
{
    uint32_t begin = time_us();
    for (;;) {
        /* Atomic test-and-sleep. TIMER1 wakes us to bound abnormal waits. */
        uint32_t mask = __get_PRIMASK();
        __disable_irq();
        if (tx_produced != tx_consumed) {
            __DMB();
            *result = tx_results[tx_consumed & 1u];
            ++tx_consumed; --tx_outstanding;
            __set_PRIMASK(mask);
            return result->addressed;
        }
        if (!tx_outstanding) { result->addressed = false; __set_PRIMASK(mask); return false; }
        if ((uint32_t)(time_us() - begin) > tx_wait_bound) {
            result->addressed = NRF_RADIO->EVENTS_ADDRESS != 0;
            __set_PRIMASK(mask);
            (void)radio_disable();
            return false;
        }
        __WFI();
        __set_PRIMASK(mask);
    }
}
bool radio_rx_healthy(void)
{
    /* END cannot clear ADDRESS between this event check and captured timestamp. */
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    bool addressed = NRF_RADIO->EVENTS_ADDRESS != 0;
    uint32_t state = NRF_RADIO->STATE;
    bool ok = clock_hf_running() &&
        (state == RADIO_STATE_STATE_Rx ||
         (state == RADIO_STATE_STATE_RxIdle && NRF_RADIO->EVENTS_END)) &&
        (!addressed || (uint32_t)(radio_time() - radio_address_time()) <= 2000u);
    __set_PRIMASK(mask);
    return ok;
}
