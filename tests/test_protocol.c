#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void record(protocol_stats_t *s, uint32_t sequence)
{
    uint8_t packet[32];
    protocol_encode(packet, sequence);
    protocol_record(s, packet, true);
}

int main(void)
{
    uint8_t packet[32];
    const uint32_t sequences[] = {0, 1, 255, 256, 0x12345678, 0xfffffffe, 0xffffffff};
    for (unsigned n = 0; n < sizeof sequences / sizeof sequences[0]; ++n) {
        uint32_t result = 0;
        protocol_encode(packet, sequences[n]);
        assert(protocol_decode(packet, &result) && result == sequences[n]);
        for (unsigned byte = 0; byte < 32; ++byte)
            for (unsigned bit = 0; bit < 8; ++bit) {
                packet[byte] ^= (uint8_t)(1u << bit);
                assert(!protocol_decode(packet, &result));
                packet[byte] ^= (uint8_t)(1u << bit);
            }
    }
    /* Independent golden vector, not just encoder/decoder round trips. */
    protocol_encode(packet, 0x12345678);
    const uint8_t golden[] = {0x78, 0x56, 0x34, 0x12, 0xa9, 0x62, 0x3f, 0x7c};
    assert(memcmp(packet, golden, sizeof golden) == 0);
    protocol_stats_t s = {0};
    record(&s, 0xfffffffe);
    record(&s, 0xffffffff);
    record(&s, 0);
    record(&s, 4);
    record(&s, 4);
    assert(s.valid == 5 && s.gaps == 3 && s.duplicates == 1 && s.backwards == 0);
    assert(s.first_sequence == 0xfffffffe && s.observed_expected == 7);
    assert(s.observed_gaps == 3);
    record(&s, 0); /* Transmitter reboot: rebase instead of counting billions lost. */
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
    puts("Protocol: golden payload, all single-bit corruptions, sequence wrap/gaps/restart passed");
}
