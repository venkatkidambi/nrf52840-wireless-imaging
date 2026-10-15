#ifndef IMAGING_EXPERIMENT_H
#define IMAGING_EXPERIMENT_H
#include <stdint.h>
#ifndef RADIO_FREQ_MHZ
#define RADIO_FREQ_MHZ 2402
#endif
#ifndef TX_INTERVAL_MS
#define TX_INTERVAL_MS 10
#endif
#ifndef TX_PACKET_COUNT
#define TX_PACKET_COUNT 10000
#endif
/* Hardware-supported proprietary channels, in integer MHz. */
#if RADIO_FREQ_MHZ < 2360 || RADIO_FREQ_MHZ > 2500
#error "RADIO_FREQ_MHZ must be an integer from 2360 through 2500"
#endif
/* Bound multiplication and modular deadlines; 10 ms preserves the old default. */
#if TX_INTERVAL_MS < 1 || TX_INTERVAL_MS > 60000
#error "TX_INTERVAL_MS must be an integer from 1 through 60000"
#endif
#if TX_PACKET_COUNT < 0 || TX_PACKET_COUNT > 0xffffffffULL
#error "TX_PACKET_COUNT must be 0 (continuous) through 4294967295"
#endif
#if RADIO_FREQ_MHZ < 2400
#define RADIO_FREQ_BASE_MHZ 2360
#define RADIO_FREQ_MAP_LOW 1
#else
#define RADIO_FREQ_BASE_MHZ 2400
#define RADIO_FREQ_MAP_LOW 0
#endif
#define RADIO_FREQ_OFFSET (RADIO_FREQ_MHZ - RADIO_FREQ_BASE_MHZ)
#define TX_INTERVAL_US ((uint32_t)TX_INTERVAL_MS * 1000u)
#define RADIO_POWER_DBM 0
#define EXP_STRING_IMPL(x) #x
#define EXP_STRING(x) EXP_STRING_IMPL(x)
#define EXP_CONFIG_TEXT "freq_mhz=" EXP_STRING(RADIO_FREQ_MHZ) \
    " tx_interval_ms=" EXP_STRING(TX_INTERVAL_MS) \
    " packet_target=" EXP_STRING(TX_PACKET_COUNT) " tx_power_dbm=0"
#endif
