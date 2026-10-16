#include "image_radio.h"
#include "radio.h"
#include "radio_timing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t clock_tick, end_tick, response_at, response_end;
static unsigned turns;
static bool listening, pending, owns_dma, lose_reply, corrupt_reply;
static const uint8_t *owned;
static uint8_t saved[IMAGE_WIRE_BYTES], response[IMAGE_WIRE_BYTES];
static radio_rx_handler_t handler;
static radio_tx_result_t result;
uint32_t radio_time(void)
{
    ++clock_tick;
    if (pending && listening && (int32_t)(clock_tick - response_at) >= 0) {
        pending = false;
        handler(response, true, response_end);
    }
    return clock_tick;
}
uint32_t radio_end_time(void) { return end_tick; }
bool radio_disable(void) { listening = false; return true; }
bool radio_receive(radio_rx_handler_t h)
{ handler = h; listening = true; clock_tick += 50; ++turns; return true; }
bool radio_rx_healthy(void) { return listening; }
bool radio_tx_enable(void)
{ listening = false; clock_tick += 50; ++turns; return true; }
bool radio_tx_submit(const uint8_t *packet, bool scheduled, uint32_t deadline)
{
    assert(!listening && !owns_dma && ((uintptr_t)packet % 4 == 0));
    if (scheduled && (int32_t)(deadline - clock_tick) > 0) clock_tick = deadline;
    result.start = clock_tick; result.address = clock_tick + 24;
    owned = packet; memcpy(saved, packet, sizeof saved); owns_dma = true;
    return true;
}
bool radio_tx_wait(radio_tx_result_t *out)
{
    assert(owns_dma && !memcmp(owned, saved, sizeof saved)); owns_dma = false;
    clock_tick += 1024; result.end = end_tick = clock_tick;
    result.addressed = true; *out = result;
    if (saved[1] == IMG_START) {
        image_encode(response, corrupt_reply ? IMG_COMMIT_ACK : IMG_READY,
                     image_get32(saved+2), 0, 0, NULL, 0);
        response_end = end_tick + IMAGE_GUARD_US + 1024;
        response_at = response_end; pending = !lose_reply;
    }
    return true;
}
int main(void)
{
    uint8_t packet[IMAGE_WIRE_BYTES], reply[IMAGE_WIRE_BYTES], metadata[20];
    image_metadata(metadata, 123);
    image_encode(packet, IMG_START, 7, 0, 0, metadata, sizeof metadata);
    clock_tick = 0xffff0000u;
    assert(image_radio_exchange(NULL, packet, reply));
    assert(reply[1] == IMG_READY && (uint32_t)(clock_tick - response_end) >= IMAGE_GUARD_US);
    assert(image_radio_turn_max >= 50 && turns == 2);
    lose_reply = true;
    uint32_t start = clock_tick;
    assert(!image_radio_exchange(NULL, packet, reply));
    assert((uint32_t)(clock_tick - start) >= IMAGE_REPLY_TIMEOUT_US);
    assert((uint32_t)(clock_tick - start) < IMAGE_REPLY_TIMEOUT_US + 3000);
    lose_reply = false; corrupt_reply = true;
    assert(!image_radio_exchange(NULL, packet, reply)); /* Wrong response type cannot truncate window. */

    assert(image_radio_listen());
    for (unsigned i = 0; i < 9; ++i) {
        memset(packet, (int)i, sizeof packet); handler(packet, true, i);
        memset(packet, 0xee, sizeof packet); /* DMA owner immediately reuses source. */
    }
    assert(image_radio_drops == 2);
    handler(packet, false, 99); assert(image_radio_crc_errors == 1);
    uint32_t end;
    for (unsigned i = 0; i < 7; ++i) {
        assert(image_radio_pop(reply, &end) && end == i);
        for (unsigned n = 0; n < sizeof reply; ++n) assert(reply[n] == i);
    }
    assert(!image_radio_pop(reply, &end));
    puts("Image radio adapter: direction guard, wrap, bounded timeout, type validation, queue ownership/drop, DMA alignment passed");
}
