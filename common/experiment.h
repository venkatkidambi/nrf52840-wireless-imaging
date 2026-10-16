#ifndef IMAGING_EXPERIMENT_H
#define IMAGING_EXPERIMENT_H
#include <stdint.h>
#ifndef IMAGE_MODE
#define IMAGE_MODE 0
#endif
#if IMAGE_MODE != 0 && IMAGE_MODE != 1
#error "IMAGE_MODE must be 0 or 1"
#endif
#if IMAGE_MODE && (APP_PAYLOAD_SIZE != 240 || TX_PACKET_COUNT != 434 || TX_INTERVAL_US > 3000)
#error "Image mode requires payload 240, packet count 434 and interval <= 3000 us"
#endif
#ifndef RADIO_FREQ_MHZ
#define RADIO_FREQ_MHZ 2440
#endif
#ifndef APP_PAYLOAD_SIZE
#define APP_PAYLOAD_SIZE 32
#endif
#ifndef RUN_ID
#define RUN_ID 1
#endif
#ifndef TX_PACKET_COUNT
#define TX_PACKET_COUNT 10000
#endif
#ifndef TX_INTERVAL_US
#ifdef TX_INTERVAL_MS
#if TX_INTERVAL_MS < 1 || TX_INTERVAL_MS > 60000
#error "TX_INTERVAL_MS must be 1..60000"
#endif
#define TX_INTERVAL_US (TX_INTERVAL_MS * 1000u)
#else
#define TX_INTERVAL_US 10000
#endif
#endif
#ifndef TX_INTERVAL_MS
#define TX_INTERVAL_MS (TX_INTERVAL_US / 1000u)
#endif
/* No S0/LENGTH/S1. Sequence and run tag are included in STATLEN. */
#define PROTOCOL_HEADER_SIZE 8u
#define RADIO_PACKET_SIZE (APP_PAYLOAD_SIZE + PROTOCOL_HEADER_SIZE)
#define RADIO_ON_AIR_SIZE (RADIO_PACKET_SIZE + 1u + 5u + 2u)
#define RADIO_AIRTIME_US (RADIO_ON_AIR_SIZE * 4u)
/* Admission budget, not a measured guarantee: END handling + DMA handoff. */
#define TX_MIN_INTERVAL_US (RADIO_AIRTIME_US + 20u)
#define PROTOCOL_DATA_LIMIT 0xfffffff0u
#if RADIO_FREQ_MHZ < 2360 || RADIO_FREQ_MHZ > 2500
#error "RADIO_FREQ_MHZ must be 2360..2500"
#endif
#if APP_PAYLOAD_SIZE < 32 || APP_PAYLOAD_SIZE > 247
#error "APP_PAYLOAD_SIZE must be 32..247 (STATLEN minus eight-byte header)"
#endif
#if TX_INTERVAL_US < 0 || TX_INTERVAL_US > 60000000 || (TX_INTERVAL_US != 0 && TX_INTERVAL_US < TX_MIN_INTERVAL_US)
#error "TX_INTERVAL_US infeasible: need airtime + 20 us, or 0 for maximum mode"
#endif
#if TX_PACKET_COUNT < 0 || TX_PACKET_COUNT > PROTOCOL_DATA_LIMIT
#error "TX_PACKET_COUNT exceeds reserved telemetry sequence boundary"
#endif
#if RUN_ID < 0 || RUN_ID > 0xffffffffULL
#error "RUN_ID must be a uint32"
#endif
#if RADIO_FREQ_MHZ < 2400
#define RADIO_FREQ_BASE_MHZ 2360
#define RADIO_FREQ_MAP_LOW 1
#else
#define RADIO_FREQ_BASE_MHZ 2400
#define RADIO_FREQ_MAP_LOW 0
#endif
#define RADIO_FREQ_OFFSET (RADIO_FREQ_MHZ - RADIO_FREQ_BASE_MHZ)
#define RADIO_POWER_DBM 0
#endif
