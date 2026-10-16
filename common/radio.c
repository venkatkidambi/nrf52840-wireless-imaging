#include "radio.h"
#include "clock.h"
#include "radio_timing.h"
#include "nrf.h"

/* Each array element is independently aligned, including the 255-byte case. */
static struct { uint8_t bytes[RADIO_PACKET_SIZE]; } __attribute__((aligned(4))) dma[2];
static unsigned active;
static radio_rx_handler_t rx_handler;
static volatile bool tx_done;
static radio_tx_result_t tx_result;
static uint32_t tx_wait_bound;
volatile uint32_t radio_rx_events;

bool radio_disable(void)
{
    NVIC_DisableIRQ(RADIO_IRQn);
    NRF_RADIO->INTENCLR = 0xffffffffu;
    radio_timing_cancel();
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
bool radio_tx_submit(const uint8_t *packet, bool scheduled, uint32_t deadline)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    clear_packet_events();
    tx_done = false;
    NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)packet;
    __DMB();
    int32_t remaining = scheduled ? (int32_t)(deadline - radio_time()) : 0;
    tx_wait_bound = (remaining > 0 ? (uint32_t)remaining : 0) + 3000u;
    bool on_time = true;
    if (scheduled) on_time = radio_start_at(deadline);
    else radio_start_now();
    __set_PRIMASK(mask);
    return on_time;
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
    } else {
        tx_result.start = radio_start_time();
        tx_result.address = radio_address_time();
        tx_result.end = end;
        tx_result.addressed = NRF_RADIO->EVENTS_ADDRESS != 0;
        NRF_RADIO->EVENTS_END = 0;
        __DMB();
        tx_done = true;
    }
    __DSB();
}
bool radio_tx_wait(radio_tx_result_t *result)
{
    uint32_t begin = time_us();
    for (;;) {
        /* Atomic test-and-sleep. TIMER1 wakes us to bound abnormal waits. */
        __disable_irq();
        if (tx_done) {
            __enable_irq();
            *result = tx_result;
            return result->addressed;
        }
        __WFI();
        __enable_irq();
        if ((uint32_t)(time_us() - begin) > tx_wait_bound) {
            result->addressed = NRF_RADIO->EVENTS_ADDRESS != 0;
            (void)radio_disable();
            return false;
        }
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
