#include "image_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t source[IMAGE_BYTES];
static struct { uint32_t before; uint8_t pixels[IMAGE_BYTES]; uint32_t after; } guarded;
static image_receiver_t rx;
static uint32_t tick, sent[IMAGE_PACKETS], stats[IMAGE_STATS_WORDS];
static unsigned loss_mode, start_loss, poll_loss, feedback_loss, complete_loss, commit_loss, commit_ack_loss;
static uint8_t wire[IMAGE_WIRE_BYTES], reply[IMAGE_WIRE_BYTES];
static unsigned stale_feedback;
static uint32_t measured_now(void *context) { (void)context; return ++tick; }
static void reset(void)
{
    memset(&guarded, 0xa5, sizeof guarded);
    image_receiver_init(&rx, guarded.pixels, 0x12345678);
    memset(sent, 0, sizeof sent); memset(stats, 0, sizeof stats);
    tick = 0xffff0000u; loss_mode = stale_feedback = 0;
    rx.clock = measured_now;
    start_loss = poll_loss = feedback_loss = complete_loss = commit_loss = commit_ack_loss = 0;
}
static uint32_t now(void *context) { (void)context; return tick; }
static bool send(void *context, const uint8_t *p)
{
    (void)context;
    image_packet_t packet;
    assert(image_decode(p, IMAGE_WIRE_BYTES, &packet));
    tick += 1500;
    image_receiver_tick(&rx, tick);
    if (packet.type == IMG_DATA) {
        unsigned n = ++sent[packet.index];
        if ((loss_mode == 1 && packet.index % 7 == 0 && n <= 2) ||
            (loss_mode == 2 && packet.index == IMAGE_PACKETS - 1) ||
            (loss_mode == 3 && packet.index == IMAGE_PACKETS - 1 && n == 1)) return true;
    }
    assert(!image_receiver_packet(&rx, p, IMAGE_WIRE_BYTES, tick, reply));
    return true;
}
static bool exchange(void *context, const uint8_t *p, uint8_t *out)
{
    (void)context;
    tick += 1024;
    bool drop = false;
    if (p[1] == IMG_START && start_loss) { --start_loss; drop = true; }
    if (p[1] == IMG_END && poll_loss) { --poll_loss; drop = true; }
    if (p[1] == IMG_COMMIT && commit_loss) { --commit_loss; drop = true; }
    if (!drop) {
        assert(image_receiver_packet(&rx, p, IMAGE_WIRE_BYTES, tick, out));
        if (out[1] == IMG_MISSING && feedback_loss) { --feedback_loss; drop = true; }
        if (out[1] == IMG_COMPLETE && complete_loss) { --complete_loss; drop = true; }
        if (out[1] == IMG_COMMIT_ACK && commit_ack_loss) { --commit_ack_loss; drop = true; }
    }
    tick += drop ? image_reply_timeout(p[1]) : IMAGE_REQUEST_GUARD_US + IMAGE_REPLY_GUARD_US + 1024;
    image_receiver_tick(&rx, tick);
    if (!drop && out[1] == IMG_MISSING && stale_feedback) {
        --stale_feedback;
        image_put16(out + 6, image_get16(out + 6) + 1u); /* Future/out-of-order reply. */
    }
    return !drop;
}
static image_link_t link = {NULL, now, send, exchange, NULL};
static void canaries(void)
{ assert(guarded.before == 0xa5a5a5a5 && guarded.after == 0xa5a5a5a5); }
static void start(void)
{
    uint8_t body[20]; image_metadata(body, image_crc32(source, sizeof source));
    image_encode(wire, IMG_START, rx.frame, 0, 0, body, sizeof body);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    assert(reply[1] == IMG_READY);
}
static void data(unsigned i)
{
    image_encode(wire, IMG_DATA, rx.frame, 0, (uint16_t)i,
                 source + i * IMAGE_DATA_BYTES, image_segment_length(i));
    assert(!image_receiver_packet(&rx, wire, sizeof wire, ++tick, reply));
}
static uint32_t reference_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc;
}
int main(void)
{
    image_generate(source);
    assert(image_crc32(source, sizeof source) == 653244199u);
    uint8_t random[1025]; uint32_t seed = 7;
    for (unsigned i = 0; i < sizeof random; ++i) {
        seed = seed * 1664525u + 1013904223u; random[i] = (uint8_t)(seed >> 24);
    }
    for (unsigned i = 0; i <= 1024; ++i) {
        uint32_t crc = image_crc_update(seed, random + 1, i); /* Unaligned input. */
        assert(crc == reference_crc(seed, random + 1, i));
        unsigned split = i / 3;
        assert(crc == image_crc_update(image_crc_update(seed, random + 1, split), random + 1 + split, i - split));
    }
    for (unsigned type = IMG_START; type <= IMG_COMMIT; ++type) {
        uint32_t timeout = image_reply_timeout(type);
        assert(timeout * image_control_tries(type) == IMAGE_REPLY_TIMEOUT_US * IMAGE_CONTROL_TRIES);
        assert(image_response_in_time(type, timeout - 10001));
        assert(!image_response_in_time(type, timeout - 10000));
        assert(!image_response_in_time(type, timeout));
    }
    assert(image_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    assert(IMAGE_PACKETS == 434 && IMAGE_BITMAP_BYTES == 55);
    assert(image_segment_length(433) == 212 && image_segment_length(434) == 0);
    uint8_t body[20]; image_metadata(body, 0x11223344);
    image_encode(wire, IMG_START, 0x12345678, 0, 0, body, 20);
    const uint8_t golden[] = {1, 1, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0, 20, 0,
        0x40, 1, 0x40, 1, 0, 0x90, 1, 0, 0xec, 0, 0xb2, 1, 0x44, 0x33, 0x22, 0x11, 1, 0, 0, 0};
    assert(!memcmp(wire, golden, sizeof golden));
    image_packet_t parsed;
    assert(image_decode(wire, sizeof wire, &parsed));
    for (size_t n = 0; n < IMAGE_WIRE_BYTES; ++n) assert(!image_decode(wire, n, &parsed));
    for (unsigned i = 0; i < 20; ++i) if (i < 12 || i >= 16) {
        wire[12+i] ^= 1; assert(!image_decode(wire, sizeof wire, &parsed)); wire[12+i] ^= 1;
    }
    wire[247] = 1; assert(!image_decode(wire, sizeof wire, &parsed));

    reset(); start();
    /* Repeated START never erases progress. Final segment first, then reverse. */
    data(433); start(); assert(rx.stats[IM_RECEIVED] == 1);
    for (int i = 432; i >= 0; --i) { data((unsigned)i); data((unsigned)i); }
    assert(rx.stats[IM_RECEIVED] == IMAGE_PACKETS && rx.stats[IM_DUPLICATES] == 433);
    assert(!memcmp(source, guarded.pixels, IMAGE_BYTES)); canaries();
    image_encode(wire, IMG_END, rx.frame, 0, 0, NULL, 0);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply) && reply[1] == IMG_COMPLETE);
    assert(rx.phases[ICR_VERIFY].count == 1 && rx.phases[ICR_COMPLETE_PREP].count == 1);
    /* Complete remains immutable and duplicate END resends ACK. */
    data(0); assert(!memcmp(source, guarded.pixels, IMAGE_BYTES));
    image_encode(wire, IMG_END, rx.frame, 0, 0, NULL, 0);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply) && reply[1] == IMG_COMPLETE);
    assert(rx.phases[ICR_VERIFY].count == 1 && rx.phases[ICR_COMPLETE_PREP].count == 2);

    reset(); start(); data(0);
    image_encode(wire, IMG_END, rx.frame, 0, 0, NULL, 0);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    assert(image_decode(reply, sizeof reply, &parsed) && parsed.type == IMG_MISSING);
    assert(!image_bit(parsed.body, 0) && image_bit(parsed.body, 433) && !image_bit(parsed.body, 434));
    reply[12+54] |= 0x80; assert(!image_decode(reply, sizeof reply, &parsed));
    image_encode(wire, IMG_DATA, rx.frame, 0, 434, source, 1);
    assert(!image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    image_encode(wire, IMG_DATA, rx.frame, 0, 433, source, 236);
    assert(!image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    image_encode(wire, IMG_DATA, rx.frame + 1, 0, 1, source, 236);
    assert(!image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    assert(rx.stats[IM_RECEIVED] == 1); canaries();
    image_metadata(body, rx.crc ^ 1u);
    image_encode(wire, IMG_START, rx.frame, 0, 0, body, sizeof body);
    assert(!image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    assert(rx.stats[IM_RECEIVED] == 1); /* Conflicting START cannot rebase a frame. */
    image_receiver_tick(&rx, tick + IMAGE_IDLE_TIMEOUT_US);
    assert(rx.failed && !rx.committed);

    reset();
    for (unsigned i = 0; i < IMAGE_TX_PHASE_WORDS; ++i) stats[IMAGE_TX_PHASE_FIRST+i] = i + 100;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && rx.verified && stats[IM_ROUNDS] == 0);
    for (unsigned i = 0; i < IMAGE_TX_PHASE_WORDS; ++i) assert(rx.stats[IMAGE_TX_PHASE_FIRST+i] == i + 100);
    assert(rx.stats[IMAGE_RX_PHASE_FIRST + 4 * ICR_VERIFY] == 1);
    assert(rx.stats[IM_INITIAL_MISSING] == 0 && !memcmp(source, guarded.pixels, IMAGE_BYTES));

    reset(); loss_mode = 1; start_loss = poll_loss = feedback_loss = complete_loss = commit_loss = 2;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && stats[IM_ROUNDS] == 2 && stats[IM_RETRANSMITS] == 124);
    assert(rx.stats[IM_INITIAL_MISSING] == 62 && stats[IM_CONTROL_RETRIES] == 10);
    for (unsigned i = 0; i < IMAGE_PACKETS; ++i) assert(sent[i] == (i % 7 ? 1u : 3u));
    assert(!memcmp(source, guarded.pixels, IMAGE_BYTES)); canaries();

    reset(); loss_mode = 3; feedback_loss = 1; stale_feedback = 1;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && stats[IM_ROUNDS] == 1 && stats[IM_RETRANSMITS] == 1);
    assert(rx.stats[IM_INITIAL_MISSING] == 1 && sent[433] == 2);
    assert(stats[IM_CONTROL_RETRIES] == 2);
    assert(rx.phases[ICR_VERIFY].count == 1);
    assert(rx.phases[ICR_FEEDBACK_PREP].count == 3); /* Duplicate END is idempotent. */
    assert(rx.phases[ICR_COMPLETE_PREP].count == 1);
    assert(!memcmp(source, guarded.pixels, IMAGE_BYTES));

    reset(); loss_mode = 3; feedback_loss = image_control_tries(IMG_END);
    assert(!image_transmit(&link, source, rx.frame, stats));
    assert(!rx.verified && rx.failed && stats[IM_CONTROL_RETRIES] == IMAGE_CONTROL_TRIES);
    assert((uint32_t)(tick - 0xffff0000u) < IMAGE_FRAME_TIMEOUT_US);

    reset(); complete_loss = IMAGE_CONTROL_TRIES;
    assert(!image_transmit(&link, source, rx.frame, stats));
    assert(rx.verified && !rx.committed && rx.failed); /* No USB success. */
    reset(); commit_ack_loss = image_control_tries(IMG_COMMIT);
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && rx.stats[IM_STATUS] == IMAGE_OK);
    reset(); commit_loss = image_control_tries(IMG_COMMIT);
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(!rx.committed); image_receiver_tick(&rx, tick + IMAGE_IDLE_TIMEOUT_US); assert(rx.failed);
    reset(); loss_mode = 2;
    assert(!image_transmit(&link, source, rx.frame, stats));
    assert(!rx.verified && !rx.committed && stats[IM_ROUNDS] == IMAGE_ROUNDS);
    assert(stats[IM_RETRANSMITS] == IMAGE_ROUNDS); canaries();
    reset(); start_loss = image_control_tries(IMG_START);
    assert(!image_transmit(&link, source, rx.frame, stats)); assert(!rx.active);

    reset(); start();
    for (unsigned i = 0; i < IMAGE_PACKETS; ++i) data(i);
    guarded.pixels[IMAGE_BYTES-1] ^= 1;
    image_encode(wire, IMG_END, rx.frame, 0, 0, NULL, 0);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
    assert(reply[1] == IMG_ABORT && rx.failed && !rx.verified);
    uint8_t error[4]; image_put32(error, IMAGE_ERR_RETRIES);
    image_encode(wire, IMG_ABORT, rx.frame, IMAGE_ROUNDS + 1u, 0, error, sizeof error);
    (void)image_receiver_packet(&rx, wire, sizeof wire, tick, reply);
    assert(rx.stats[IM_STATUS] == IMAGE_ERR_CHECKSUM); /* Keep the original cause. */
    for (unsigned boundary = 0; boundary < 2; ++boundary) {
        reset(); rx.frame = boundary ? UINT32_MAX : 0;
        assert(image_transmit(&link, source, rx.frame, stats) && rx.committed);
        image_metadata(body, rx.crc);
        image_encode(wire, IMG_START, rx.frame ^ 1u, 0, 0, body, sizeof body);
        assert(!image_receiver_packet(&rx, wire, sizeof wire, tick, reply));
        assert(rx.committed && !memcmp(source, guarded.pixels, IMAGE_BYTES));
    }
    puts("Image: serialization, bounds, reorder/duplicates, bitmap, selective loss, lost controls/ACKs, bounded failure, CRC passed");
}
