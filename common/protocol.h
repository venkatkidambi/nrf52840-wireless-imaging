#ifndef IMAGING_PROTOCOL_H
#define IMAGING_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#define PROTOCOL_PAYLOAD_SIZE 32u
/* Bytes 0..3: little-endian sequence. Bytes 4..31: deterministic check. */
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
#endif
