#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void record(protocol_stats_t *s, uint32_t sequence)
{
    uint8_t packet[PROTOCOL_PAYLOAD_SIZE];
    protocol_encode(packet, sequence);
    protocol_record(s, packet, true);
}

int main(void)
{
    uint8_t packet[PROTOCOL_PAYLOAD_SIZE];
    const uint32_t sequences[] = {0, 1, 255, 256, 0x12345678, PROTOCOL_DATA_LIMIT-2, PROTOCOL_DATA_LIMIT-1};
    for (unsigned n = 0; n < sizeof sequences / sizeof sequences[0]; ++n) {
        uint32_t result = 0;
        protocol_encode(packet, sequences[n]);
        assert(protocol_decode(packet, &result) && result == sequences[n]);
        for (unsigned byte = 0; byte < PROTOCOL_PAYLOAD_SIZE; ++byte)
            for (unsigned bit = 0; bit < 8; ++bit) {
                packet[byte] ^= (uint8_t)(1u << bit);
                assert(!protocol_decode(packet, &result));
                packet[byte] ^= (uint8_t)(1u << bit);
            }
    }
    /* Independent golden vector, not just encoder/decoder round trips. */
    protocol_encode(packet, 0x12345678);
    const uint8_t golden[] = {0x78, 0x56, 0x34, 0x12, RUN_ID & 255, (RUN_ID >> 8) & 255, (RUN_ID >> 16) & 255, RUN_ID >> 24,
                              0x35, 0xf6, 0xb3, 0x88, 0x81, 0x8a, 0x07, 0x04};
    assert(memcmp(packet, golden, sizeof golden) == 0);
    protocol_stats_t s = {0};
    record(&s, 0);
    record(&s, 1);
    record(&s, 2);
    record(&s, 6);
    record(&s, 6);
    assert(s.valid == 5 && s.gaps == 3 && s.duplicates == 1 && s.backwards == 0);
    assert(s.first_sequence == 0 && s.observed_expected == 7);
    assert(s.observed_gaps == 3);
    record(&s, 0); /* Restart is reported, not treated as a huge gap. */
    record(&s, 1);
    assert(s.backwards == 1 && s.gaps == 3 && s.last_sequence == 1);
    protocol_record(&s, packet, false);
    packet[31] ^= 1;
    protocol_record(&s, packet, true);
    assert(s.crc_errors == 1 && s.payload_errors == 1 && s.valid == 7 && s.last_sequence == 1);
    s = (protocol_stats_t){0};
    record(&s, 100); /* Leading unseen packets are not inferred. */
    protocol_record(&s, packet, false);
    record(&s, 102);
    record(&s, 102);
    assert(s.first_sequence == 100 && s.observed_expected == 3);
    assert(s.observed_gaps == 1 && s.gaps == 1 && s.crc_errors == 1);
    assert(s.valid == 3 && s.duplicates == 1); /* CRC is not added to gaps. */
    assert(protocol_gap_ppm(&s) == 333333);
    s.observed_expected = UINT64_MAX;
    s.observed_gaps = UINT64_MAX - 1u;
    assert(protocol_gap_ppm(&s) == 999999);
    s.observed_gaps = 0;
    assert(protocol_gap_ppm(&s) == 0);
    s.backwards = 1;
    assert(protocol_gap_ppm(&s) == 0);
    /* Canary boundaries cover every supported build size, including 255 DMA bytes. */
    struct { uint8_t before[4], bytes[PROTOCOL_PAYLOAD_SIZE], after[4]; } guarded;
    memset(&guarded, 0x5a, sizeof guarded);
    protocol_encode(guarded.bytes, 42);
    for (unsigned i = 0; i < 4; ++i)
        assert(guarded.before[i] == 0x5a && guarded.after[i] == 0x5a);
    uint32_t n;
    protocol_encode(packet, PROTOCOL_DATA_LIMIT);
    assert(!protocol_decode(packet, &n));
    protocol_encode(packet, 0);
    packet[4] ^= 1;
    assert(!protocol_decode(packet, &n));
    uint32_t words[24], received[24] = {0}, mask = 0;
    for (unsigned i = 0; i < 24; ++i) words[i] = i * 0x1020304u;
    for (unsigned page = 0; page < 3; ++page) {
        protocol_encode_telemetry(guarded.bytes, words, page);
        assert(!protocol_decode(guarded.bytes, &n));
        assert(protocol_decode_telemetry(guarded.bytes, received, &mask));
        for (unsigned i = 0; i < 4; ++i)
            assert(guarded.before[i] == 0x5a && guarded.after[i] == 0x5a);
    }
    assert(mask == PROTOCOL_TELEMETRY_MASK && !memcmp(words, received, sizeof words));
    guarded.bytes[8] ^= 1;
    assert(!protocol_decode_telemetry(guarded.bytes, received, &mask));
    guarded.bytes[8] ^= 1;
    guarded.bytes[4] ^= 1;
    assert(!protocol_decode_telemetry(guarded.bytes, received, &mask));
    protocol_encode(packet, 7);
    assert(!protocol_decode_telemetry(packet, received, &mask));
    puts("Protocol: pattern, all bit corruptions, boundaries, run tags, sequence gaps/restarts, telemetry passed");
}
