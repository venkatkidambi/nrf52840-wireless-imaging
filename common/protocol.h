#ifndef IMAGING_PROTOCOL_H
#define IMAGING_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include "experiment.h"
#define PROTOCOL_PAYLOAD_SIZE RADIO_PACKET_SIZE
/* Bytes 0..3 sequence, 4..7 run tag; the remainder is application data. */
void protocol_encode(uint8_t out[PROTOCOL_PAYLOAD_SIZE], uint32_t sequence);
bool protocol_decode(const uint8_t in[PROTOCOL_PAYLOAD_SIZE], uint32_t *sequence);
typedef struct {
    uint32_t valid, crc_errors, payload_errors, gaps, duplicates, backwards;
    uint32_t last_sequence;
    uint32_t first_sequence;
    uint64_t observed_expected, observed_gaps;
    bool have_sequence;
} protocol_stats_t;
void protocol_record(protocol_stats_t *s, const uint8_t *packet, bool crc_ok);
/* Floor of the observed gap fraction in parts per million; zero if undefined.
 * Callers must also expose have_sequence && !backwards as the validity flag. */
uint32_t protocol_gap_ppm(const protocol_stats_t *s);
/* Three CRC-protected final-stat pages, eight words each; never application data. */
#define PROTOCOL_TELEMETRY_WORDS 24u
#define PROTOCOL_TELEMETRY_MASK 7u
enum { TX_ATTEMPTS, TX_STARTS, TX_COMPLETIONS, TX_MISSED,
       TX_DELAY_MIN, TX_DELAY_MEAN, TX_DELAY_MAX, TX_SPACING_N,
       TX_SPACING_MIN, TX_SPACING_MEAN, TX_SPACING_MAX, TX_TURN_N,
       TX_TURN_MIN, TX_TURN_MEAN, TX_TURN_MAX, TX_DURATION_LO,
       TX_DURATION_HI, TX_FAILURES, TX_COMPLETE };
void protocol_encode_telemetry(uint8_t *out, const uint32_t *words, unsigned page);
bool protocol_decode_telemetry(const uint8_t *in, uint32_t *words, uint32_t *mask);
#endif
