#include "experiment.h"
#if IMAGE_MODE
#include "image_radio.h"
#include "radio.h"
#include "radio_timing.h"
#include "nrf.h"
#include <string.h>

_Static_assert(RADIO_PACKET_SIZE == IMAGE_WIRE_BYTES, "Image DMA size");
typedef struct { _Alignas(4) uint8_t wire[IMAGE_WIRE_BYTES]; uint32_t end; } slot_t;
static slot_t queue[8];
static volatile unsigned head, tail;
static bool transmitting;
static uint32_t prior_start;
static bool have_start;
uint32_t image_radio_turn_max;
volatile uint32_t image_radio_crc_errors, image_radio_drops;
static _Alignas(4) uint8_t tx_dma[IMAGE_WIRE_BYTES];

static void received(const uint8_t *p, bool crc, uint32_t end)
{
    if (!crc) { ++image_radio_crc_errors; return; }
    unsigned next = (head + 1u) % 8u;
    if (next == tail) { ++image_radio_drops; return; }
    memcpy(queue[head].wire, p, IMAGE_WIRE_BYTES);
    queue[head].end = end;
    __DMB(); head = next;
}
bool image_radio_pop(uint8_t out[IMAGE_WIRE_BYTES], uint32_t *end)
{
    if (head == tail) return false;
    __DMB();
    memcpy(out, queue[tail].wire, IMAGE_WIRE_BYTES);
    *end = queue[tail].end;
    __DMB(); tail = (tail + 1u) % 8u;
    return true;
}
static void measured_turn(uint32_t begin)
{
    uint32_t dt = radio_time() - begin;
    if (dt > image_radio_turn_max) image_radio_turn_max = dt;
}
bool image_radio_listen(void)
{
    uint32_t start = radio_time();
    bool ok = radio_receive(received);
    measured_turn(start); transmitting = false; have_start = false;
    return ok;
}
void image_radio_guard(uint32_t end)
{
    /* Busy wait only for the short direction guard; TIMER/PPI schedules DATA.
     * Sleep's 1 ms wake quantum must not postpone a control reply arbitrarily. */
    while ((uint32_t)(radio_time() - end) < IMAGE_GUARD_US) {}
}
bool image_radio_send(const uint8_t *wire)
{
    if (!transmitting) {
        uint32_t start = radio_time();
        if (!radio_tx_enable()) return false;
        measured_turn(start); transmitting = true; have_start = false;
    }
    memcpy(tx_dma, wire, sizeof tx_dma);
    bool scheduled = have_start && TX_INTERVAL_US;
    (void)radio_tx_submit(tx_dma, scheduled, prior_start + TX_INTERVAL_US);
    radio_tx_result_t result;
    if (!radio_tx_wait(&result)) { transmitting = false; have_start = false; return false; }
    prior_start = result.start; have_start = true;
    return true;
}
bool image_radio_exchange(void *context, const uint8_t *request, uint8_t *reply)
{
    (void)context;
    /* Stop the previous receive before dropping stale queued responses. */
    if (!radio_disable()) return false;
    transmitting = false; head = tail = 0;
    if (!image_radio_send(request)) return false;
    uint32_t sent = radio_end_time();
    if (!image_radio_listen()) return false;
    while ((uint32_t)(radio_time() - sent) < IMAGE_REPLY_TIMEOUT_US) {
        uint32_t end;
        if (image_radio_pop(reply, &end)) {
            image_packet_t p;
            if (image_decode(reply, IMAGE_WIRE_BYTES, &p) &&
                p.frame == image_get32(request + 2) && p.token == image_get16(request + 6) &&
                (p.type == IMG_ABORT || (request[1] == IMG_START && p.type == IMG_READY) ||
                 (request[1] == IMG_END && (p.type == IMG_MISSING || p.type == IMG_COMPLETE)) ||
                 (request[1] == IMG_COMMIT && p.type == IMG_COMMIT_ACK))) {
                image_radio_guard(end);
                return true;
            }
        }
        if (!radio_rx_healthy()) return false;
    }
    return false;
}
#endif
