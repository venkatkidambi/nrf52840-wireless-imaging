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
static void reset(void)
{
    memset(&guarded, 0xa5, sizeof guarded);
    image_receiver_init(&rx, guarded.pixels, 0x12345678);
    memset(sent, 0, sizeof sent); memset(stats, 0, sizeof stats);
    tick = 0xffff0000u; loss_mode = 0;
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
            (loss_mode == 2 && packet.index == IMAGE_PACKETS - 1)) return true;
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
    tick += drop ? IMAGE_REPLY_TIMEOUT_US : 2 * IMAGE_GUARD_US + 1024;
    image_receiver_tick(&rx, tick);
    return !drop;
}
static image_link_t link = {NULL, now, send, exchange};
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
int main(void)
{
    image_generate(source);
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
    /* Complete remains immutable and duplicate END resends ACK. */
    data(0); assert(!memcmp(source, guarded.pixels, IMAGE_BYTES));
    image_encode(wire, IMG_END, rx.frame, 0, 0, NULL, 0);
    assert(image_receiver_packet(&rx, wire, sizeof wire, tick, reply) && reply[1] == IMG_COMPLETE);

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

    reset(); assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && rx.verified && stats[IM_ROUNDS] == 0);
    assert(rx.stats[IM_INITIAL_MISSING] == 0 && !memcmp(source, guarded.pixels, IMAGE_BYTES));

    reset(); loss_mode = 1; start_loss = poll_loss = feedback_loss = complete_loss = commit_loss = 2;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && stats[IM_ROUNDS] == 2 && stats[IM_RETRANSMITS] == 124);
    assert(rx.stats[IM_INITIAL_MISSING] == 62 && stats[IM_CONTROL_RETRIES] == 10);
    for (unsigned i = 0; i < IMAGE_PACKETS; ++i) assert(sent[i] == (i % 7 ? 1u : 3u));
    assert(!memcmp(source, guarded.pixels, IMAGE_BYTES)); canaries();

    reset(); complete_loss = IMAGE_CONTROL_TRIES;
    assert(!image_transmit(&link, source, rx.frame, stats));
    assert(rx.verified && !rx.committed && rx.failed); /* No USB success. */
    reset(); commit_ack_loss = IMAGE_CONTROL_TRIES;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(rx.committed && rx.stats[IM_STATUS] == IMAGE_OK);
    reset(); commit_loss = IMAGE_CONTROL_TRIES;
    assert(image_transmit(&link, source, rx.frame, stats));
    assert(!rx.committed); image_receiver_tick(&rx, tick + IMAGE_IDLE_TIMEOUT_US); assert(rx.failed);
    reset(); loss_mode = 2;
    assert(!image_transmit(&link, source, rx.frame, stats));
    assert(!rx.verified && !rx.committed && stats[IM_ROUNDS] == IMAGE_ROUNDS);
    assert(stats[IM_RETRANSMITS] == IMAGE_ROUNDS); canaries();
    reset(); start_loss = IMAGE_CONTROL_TRIES;
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
