#include "radio.h"
#include "clock.h"
#include "experiment.h"
#include "radio_timing.h"
#include "nrf.h"
#include <string.h>

/* Owned by the driver, aligned and permanently resident in SRAM for EasyDMA. */
static uint8_t dma_packet[RADIO_PAYLOAD_SIZE] __attribute__((aligned(4)));
static bool receiving, addressed;
static uint32_t address_time;

bool radio_disable(void)
{
    receiving = false;
    NRF_RADIO->SHORTS = 0;
    if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) return true;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->TASKS_DISABLE = 1;
    if (wait_event(&NRF_RADIO->EVENTS_DISABLED, 2000u)) return true;
    /* Stop any outstanding DMA even on failure. radio_init is required next. */
    NRF_RADIO->POWER = 0;
    return false;
}

bool radio_init(void)
{
    radio_timing_cancel();
    receiving = false;
    if (!clock_hf_running()) return false;
    NRF_RADIO->POWER = 0;
    __DSB();
    NRF_RADIO->POWER = 1;
    NRF_RADIO->INTENCLR = 0xffffffffu;
    NVIC_DisableIRQ(RADIO_IRQn);
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit;
    NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Default << RADIO_MODECNF0_RU_Pos;
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
    NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)dma_packet;
    return radio_disable();
}

static bool prepare(void)
{
    if (!clock_hf_running() || !radio_disable() || !NRF_RADIO->POWER) return false;
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END = 0;
    NRF_RADIO->EVENTS_ADDRESS = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;
    NRF_RADIO->EVENTS_CRCOK = 0;
    NRF_RADIO->EVENTS_CRCERROR = 0;
    NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)dma_packet;
    NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
    __DMB();
    return true;
}

bool radio_transmit(const uint8_t packet[RADIO_PAYLOAD_SIZE])
{
    if (!prepare()) return false;
    memcpy(dma_packet, packet, sizeof dma_packet);
    __DMB();
    NRF_RADIO->TASKS_TXEN = 1;
    bool ok = wait_event(&NRF_RADIO->EVENTS_END, 2000u) &&
              wait_event(&NRF_RADIO->EVENTS_DISABLED, 2000u);
    bool stopped = radio_disable();
    return ok && stopped;
}

bool radio_receive(void)
{
    if (!prepare()) { radio_timing_cancel(); return false; }
    addressed = false;
    radio_timing_rxen();
    if (!wait_event(&NRF_RADIO->EVENTS_READY, 2000u)) {
        radio_timing_cancel();
        (void)radio_disable();
        return false;
    }
    receiving = true;
    radio_timing_ready(); /* Arithmetic only after RX is listening again. */
    return true;
}

radio_result_t radio_poll(uint8_t packet[RADIO_PAYLOAD_SIZE], bool *crc_ok)
{
    if (!receiving) return RADIO_ERROR;
    if (NRF_RADIO->EVENTS_END) {
        radio_timing_end(); /* Before waiting for DISABLED or copying EasyDMA. */
        if (!wait_event(&NRF_RADIO->EVENTS_DISABLED, 2000u)) {
            radio_timing_cancel();
            (void)radio_disable();
            return RADIO_ERROR;
        }
        __DMB();
        *crc_ok = NRF_RADIO->CRCSTATUS == RADIO_CRCSTATUS_CRCSTATUS_CRCOk;
        memcpy(packet, dma_packet, sizeof dma_packet);
        receiving = false;
        return RADIO_PACKET;
    }
    /* Silence is normal. A started packet must finish within a bounded time. */
    if (!addressed && NRF_RADIO->EVENTS_ADDRESS) {
        address_time = time_us();
        addressed = true;
    }
    if (!clock_hf_running() || (addressed && (uint32_t)(time_us() - address_time) > 2000u)) {
        (void)radio_disable();
        return RADIO_ERROR;
    }
    return RADIO_IDLE;
}
