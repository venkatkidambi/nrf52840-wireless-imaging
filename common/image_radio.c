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
static bool pending, pending_data, have_data;
static unsigned tx_slot;
static radio_tx_result_t prior_data;
static measurement_t spacing, airtime, handoff;
static uint32_t missed, late_max, tx_to_rx, rx_to_tx;
static measurement_t phases[ICT_COUNT];
static uint32_t data_end, feedback_end, burst_start, burst_end;
static bool data_end_valid, feedback_pending, burst_active;
static uint16_t pending_token;
uint32_t image_radio_turn_max;
volatile uint32_t image_radio_crc_errors, image_radio_drops;
/* COMMIT finishes after the telemetry snapshot already sent to RX. Keep its
 * full local measurements available to a debugger without a new RF handshake. */
measurement_t image_radio_confirmation, image_radio_terminal_guard;
static _Alignas(4) uint8_t tx_dma[2][IMAGE_WIRE_BYTES];

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
    if (!image_radio_flush()) return false;
    uint32_t start = radio_time();
    bool ok = radio_receive(received);
    uint32_t dt = radio_time() - start;
    if (dt > tx_to_rx) tx_to_rx = dt;
    measured_turn(start); transmitting = false; have_data = false;
    if (ok && data_end_valid) measurement_add(&phases[ICT_DATA_READY], start + dt - data_end);
    data_end_valid = false;
    return ok;
}
static void guard(uint32_t end, uint32_t duration)
{
    /* Busy wait only for the short direction guard; TIMER/PPI schedules DATA.
     * Sleep's 1 ms wake quantum must not postpone a control reply arbitrarily. */
    while ((uint32_t)(radio_time() - end) < duration) {}
}
void image_radio_guard(uint32_t end) { guard(end, IMAGE_REQUEST_GUARD_US); }
bool image_radio_flush(void)
{
    if (!pending) return true;
    radio_tx_result_t result;
    if (!radio_tx_wait(&result)) {
        (void)radio_disable();
        pending = false; transmitting = false; have_data = false; return false;
    }
    pending = false;
    if (pending_data) {
        data_end = result.end; data_end_valid = true;
        if (pending_token) {
            if (!burst_active) {
                burst_start = result.start; burst_active = true;
                if (feedback_pending) {
                    measurement_add(&phases[ICT_FEEDBACK_DATA], result.start - feedback_end);
                    feedback_pending = false;
                }
            }
            burst_end = result.end;
        }
        measurement_add(&airtime, result.end - result.address + 24u);
        if (have_data) {
            measurement_add(&spacing, result.address - prior_data.address);
            measurement_add(&handoff, result.start - prior_data.end);
        }
        if (result.scheduled) {
            if (!result.on_time) ++missed;
            int32_t late = (int32_t)(result.start - result.deadline);
            if (late > 0 && (uint32_t)late > late_max) late_max = (uint32_t)late;
        }
        prior_data = result; have_data = true;
    }
    return true;
}
void image_radio_stats(uint32_t stats[IMAGE_STATS_WORDS])
{
    stats[IM_TX_TURN_MAX] = image_radio_turn_max;
    stats[IM_SPACING_N] = spacing.count;
    stats[IM_SPACING_MIN] = spacing.min; stats[IM_SPACING_MEAN] = measurement_mean(&spacing);
    stats[IM_SPACING_MAX] = spacing.max; stats[IM_MISSED] = missed;
    stats[IM_AIR_MEAN] = measurement_mean(&airtime); stats[IM_AIR_MAX] = airtime.max;
    stats[IM_HANDOFF_MEAN] = measurement_mean(&handoff); stats[IM_HANDOFF_MAX] = handoff.max;
    stats[IM_LATE_MAX] = late_max;
    stats[IM_TX_TO_RX] = tx_to_rx; stats[IM_TX_RX_TO_TX] = rx_to_tx;
    for (unsigned i = 0; i < ICT_COUNT; ++i)
        image_measure_export(stats + IMAGE_TX_PHASE_FIRST + 4u * i, &phases[i]);
}
void image_radio_rx_stats(uint32_t stats[IMAGE_STATS_WORDS])
{
    stats[IM_RX_TX_TO_RX] = tx_to_rx; stats[IM_RX_TO_TX] = rx_to_tx;
}
bool image_radio_send(const uint8_t *wire)
{
    if (wire[1] != IMG_DATA) {
        if (!image_radio_flush()) return false;
        if (burst_active) {
            measurement_add(&phases[ICT_RETRANS_BURST], burst_end - burst_start);
            burst_active = false;
        }
    }
    if (!transmitting) {
        uint32_t start = radio_time();
        if (!radio_tx_enable()) return false;
        uint32_t dt = radio_time() - start;
        if (dt > rx_to_tx) rx_to_tx = dt;
        measured_turn(start); transmitting = true; have_data = false;
    }
    tx_slot ^= 1u;
    memcpy(tx_dma[tx_slot], wire, IMAGE_WIRE_BYTES);
    if (pending) {
        if (!radio_tx_stage(tx_dma[tx_slot], TX_INTERVAL_US)) {
            (void)radio_disable(); pending = transmitting = false; return false;
        }
        if (!image_radio_flush()) return false;
    } else {
        if (!radio_tx_submit(tx_dma[tx_slot], false, 0)) return false;
    }
    pending = true; pending_data = wire[1] == IMG_DATA;
    pending_token = image_get16(wire + 6);
    /* DATA is pipelined; control callers retain synchronous direction ownership. */
    return pending_data || image_radio_flush();
}
bool image_radio_exchange(void *context, const uint8_t *request, uint8_t *reply)
{
    (void)context;
    uint32_t exchange_begin = radio_time();
    /* Stop the previous receive before dropping stale queued responses. */
    if (!image_radio_flush()) return false;
    if (!transmitting && !radio_disable()) return false;
    head = tail = 0;
    if (!image_radio_send(request)) return false;
    uint32_t sent = radio_end_time();
    if (!image_radio_listen()) return false;
    while ((uint32_t)(radio_time() - sent) < image_reply_timeout(request[1])) {
        uint32_t end;
        if (image_radio_pop(reply, &end)) {
            image_packet_t p;
            if (image_decode(reply, IMAGE_WIRE_BYTES, &p) &&
                p.frame == image_get32(request + 2) && p.token == image_get16(request + 6) &&
                (p.type == IMG_ABORT || (request[1] == IMG_START && p.type == IMG_READY) ||
                 (request[1] == IMG_END && (p.type == IMG_MISSING || p.type == IMG_COMPLETE)) ||
                 (request[1] == IMG_COMMIT && p.type == IMG_COMMIT_ACK))) {
                if (p.type == IMG_MISSING) {
                    measurement_add(&phases[ICT_FEEDBACK_WAIT], end - sent);
                    feedback_end = end; feedback_pending = true;
                } else if (p.type == IMG_COMPLETE) {
                    measurement_add(&phases[ICT_COMPLETE_WAIT], end - sent);
                }
                uint32_t before_guard = radio_time();
                /* ACK is terminal: TX disables the radio next, so there is no
                 * outgoing packet requiring the peer to return to RX. */
                if (p.type != IMG_COMMIT_ACK || IMAGE_CONTROL_PROFILE == 0)
                    guard(end, IMAGE_REPLY_GUARD_US);
                uint32_t after_guard = radio_time();
                if (p.type == IMG_COMMIT_ACK) {
                    measurement_add(&image_radio_terminal_guard, after_guard - before_guard);
                    measurement_add(&image_radio_confirmation, after_guard - exchange_begin);
                } else {
                    measurement_add(&phases[ICT_REPLY_GUARD], after_guard - before_guard);
                }
                return true;
            }
        }
        if (!radio_rx_healthy()) return false;
    }
    return false;
}
#endif
