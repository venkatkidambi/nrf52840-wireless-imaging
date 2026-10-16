#include "protocol.h"

static uint8_t pattern(uint32_t sequence, unsigned i)
{
    return (uint8_t)(0xA5u ^ (i * 29u) ^
                     ((sequence >> (8u * (i & 3u))) & 0xffu));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8u * i));
}

void protocol_encode(uint8_t out[PROTOCOL_PAYLOAD_SIZE], uint32_t sequence)
{
    put32(out, sequence);
    put32(out + 4, RUN_ID);
    for (unsigned i = 8; i < PROTOCOL_PAYLOAD_SIZE; ++i) out[i] = pattern(sequence, i);
}

bool protocol_decode(const uint8_t in[PROTOCOL_PAYLOAD_SIZE], uint32_t *sequence)
{
    uint32_t n = get32(in);
    if (get32(in + 4) != RUN_ID || n >= PROTOCOL_DATA_LIMIT) return false;
    for (unsigned i = 8; i < PROTOCOL_PAYLOAD_SIZE; ++i)
        if (in[i] != pattern(n, i)) return false;
    *sequence = n;
    return true;
}

void protocol_encode_telemetry(uint8_t *out, const uint32_t *words, unsigned page)
{
    protocol_encode(out, PROTOCOL_DATA_LIMIT + page);
    for (unsigned i = 0; i < 8; ++i) put32(out + 8 + 4*i, words[page*8+i]);
}

bool protocol_decode_telemetry(const uint8_t *in, uint32_t *words, uint32_t *mask)
{
    uint32_t page = get32(in) - PROTOCOL_DATA_LIMIT;
    if (page >= 3 || get32(in + 4) != RUN_ID) return false;
    for (unsigned i = 40; i < PROTOCOL_PAYLOAD_SIZE; ++i)
        if (in[i] != pattern(PROTOCOL_DATA_LIMIT + page, i)) return false;
    if (*mask & (1u << page))
        for (unsigned i = 0; i < 8; ++i)
            if (words[page*8+i] != get32(in + 8 + 4*i)) return false;
    for (unsigned i = 0; i < 8; ++i) words[page*8+i] = get32(in + 8 + 4*i);
    *mask |= 1u << page;
    return true;
}

void protocol_record(protocol_stats_t *s, const uint8_t *packet, bool crc_ok)
{
    uint32_t sequence;
    if (!crc_ok) { ++s->crc_errors; return; }
    if (!protocol_decode(packet, &sequence)) { ++s->payload_errors; return; }
    ++s->valid;
    if (s->have_sequence) {
        uint32_t delta = sequence - s->last_sequence;
        if (delta == 0) { ++s->duplicates; return; }
        /* Modular ordering. A TX restart is reported separately, then rebased. */
        if (delta < 0x80000000u) {
            s->gaps += delta - 1u;
            s->observed_gaps += delta - 1u;
            s->observed_expected += delta;
        }
        else ++s->backwards;
    } else {
        s->first_sequence = sequence;
        s->observed_expected = 1;
    }
    s->last_sequence = sequence;
    s->have_sequence = true;
}

uint32_t protocol_gap_ppm(const protocol_stats_t *s)
{
    if (!s->have_sequence || s->backwards || !s->observed_expected) return 0;
    uint64_t remainder = s->observed_gaps, denominator = s->observed_expected;
    if (remainder >= denominator) return 1000000u;
    uint32_t result = 0;
    /* Six decimal digits, without a potentially overflowing gaps * 1000000.
     * Report-only work; each addition is reduced before it can overflow. */
    for (unsigned place = 0; place < 6; ++place) {
        uint64_t next = 0;
        unsigned digit = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (next >= denominator - remainder) {
                next -= denominator - remainder;
                ++digit;
            } else next += remainder;
        }
        result = result * 10u + digit;
        remainder = next;
    }
    return result;
}
