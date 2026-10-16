#ifndef IMAGE_PROTOCOL_H
#define IMAGE_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "image_control.h"
#include "measure.h"

#define IMAGE_WIDTH 320u
#define IMAGE_HEIGHT 320u
#define IMAGE_BYTES (IMAGE_WIDTH * IMAGE_HEIGHT)
#define IMAGE_WIRE_BYTES 248u
#define IMAGE_HEADER_BYTES 12u
#define IMAGE_DATA_BYTES (IMAGE_WIRE_BYTES - IMAGE_HEADER_BYTES)
#define IMAGE_PACKETS ((IMAGE_BYTES + IMAGE_DATA_BYTES - 1u) / IMAGE_DATA_BYTES)
#define IMAGE_BITMAP_BYTES ((IMAGE_PACKETS + 7u) / 8u)
#define IMAGE_ROUNDS 16u
#define IMAGE_CONTROL_TRIES 8u
#define IMAGE_GUARD_US 3000u /* Legacy baseline reference; live guards in image_control.h. */
#define IMAGE_REPLY_TIMEOUT_US 120000u
#define IMAGE_SHORT_REPLY_TIMEOUT_US 20000u
#define IMAGE_IDLE_TIMEOUT_US 5000000u
#define IMAGE_FRAME_TIMEOUT_US 30000000u
#define IMAGE_STATS_WORDS 97u
#define IMAGE_TX_PHASE_FIRST 45u
#define IMAGE_TX_PHASE_WORDS 24u
#define IMAGE_RX_PHASE_FIRST 69u
#define IMAGE_TX_WORDS 13u
#define IMAGE_TX_EXTRA_FIRST 24u
#define IMAGE_TX_EXTRA_WORDS 16u
#define IMAGE_LEGACY_COMMIT_WORDS (IMAGE_TX_WORDS + IMAGE_TX_EXTRA_WORDS)
#define IMAGE_COMMIT_WORDS (IMAGE_LEGACY_COMMIT_WORDS + IMAGE_TX_PHASE_WORDS)
_Static_assert(IMAGE_COMMIT_WORDS * 4u <= IMAGE_DATA_BYTES, "COMMIT fits one packet");
enum { ICT_DATA_READY, ICT_FEEDBACK_WAIT, ICT_FEEDBACK_DATA, ICT_RETRANS_BURST,
       ICT_COMPLETE_WAIT, ICT_REPLY_GUARD, ICT_COUNT };
enum { ICR_END_DETECT, ICR_FEEDBACK_PREP, ICR_FEEDBACK_SEND, ICR_VERIFY,
       ICR_COMPLETE_PREP, ICR_COMPLETE_SEND, ICR_CONFIRM, ICR_COUNT };
_Static_assert(IMAGE_TX_PHASE_WORDS == 4u * ICT_COUNT, "TX phase schema");
_Static_assert(IMAGE_TX_PHASE_FIRST + IMAGE_TX_PHASE_WORDS == IMAGE_RX_PHASE_FIRST, "Phase ownership");
_Static_assert(IMAGE_RX_PHASE_FIRST + 4u * ICR_COUNT == IMAGE_STATS_WORDS, "RX phase schema");
static inline void image_measure_export(uint32_t *out, const measurement_t *m)
{ out[0] = m->count; out[1] = m->min; out[2] = measurement_mean(m); out[3] = m->max; }
enum { IMG_START = 1, IMG_READY, IMG_DATA, IMG_END, IMG_MISSING,
       IMG_COMPLETE, IMG_COMMIT, IMG_COMMIT_ACK, IMG_ABORT };
/* END may require full-frame CRC; START/COMMIT only produce a small reply.
 * Short timeout still exceeds the largest guards plus two packets. */
static inline uint32_t image_reply_timeout(uint8_t type)
{ return type == IMG_END ? IMAGE_REPLY_TIMEOUT_US : IMAGE_SHORT_REPLY_TIMEOUT_US; }
static inline unsigned image_control_tries(uint8_t type)
{ return IMAGE_CONTROL_TRIES * (IMAGE_REPLY_TIMEOUT_US / image_reply_timeout(type)); }
static inline bool image_response_in_time(uint8_t type, uint32_t elapsed)
{ return elapsed < image_reply_timeout(type) - 10000u; }
enum { IM_STATUS, IM_GENERATION, IM_INITIAL_US, IM_INITIAL_PACKETS,
       IM_ROUNDS, IM_RETRANSMITS, IM_TX_TOTAL_US, IM_RTT_N, IM_RTT_MIN,
       IM_RTT_MEAN, IM_RTT_MAX, IM_CONTROL_RETRIES, IM_TX_TURN_MAX,
       IM_INITIAL_MISSING, IM_RX_TOTAL_US, IM_VERIFY_US, IM_DUPLICATES,
       IM_INVALID, IM_CRC_ERRORS, IM_QUEUE_DROPS, IM_RX_TURN_MAX,
       IM_USB_US, IM_RECEIVED, IM_CRC32,
       IM_TX_CRC_US, IM_SPACING_N, IM_SPACING_MIN, IM_SPACING_MEAN, IM_SPACING_MAX,
       IM_MISSED, IM_AIR_MEAN, IM_AIR_MAX, IM_HANDOFF_MEAN, IM_HANDOFF_MAX,
       IM_PREP_MAX, IM_RETRANS_US, IM_FEEDBACK_US, IM_LATE_MAX,
       IM_TX_TO_RX, IM_TX_RX_TO_TX,
       IM_RX_CPU_MAX, IM_RX_REARM_MAX, IM_USB_CRC_US, IM_RX_TX_TO_RX, IM_RX_TO_TX };
enum { IMAGE_OK = 1, IMAGE_ERR_CHECKSUM = 2, IMAGE_ERR_TIMEOUT = 3,
       IMAGE_ERR_RADIO = 4, IMAGE_ERR_RETRIES = 5 };
typedef struct {
    uint8_t type;
    uint32_t frame;
    uint16_t token, index, length;
    const uint8_t *body;
} image_packet_t;
uint16_t image_get16(const uint8_t *p);
uint32_t image_get32(const uint8_t *p);
void image_put16(uint8_t *p, uint16_t v);
void image_put32(uint8_t *p, uint32_t v);
uint32_t image_crc_update(uint32_t crc, const uint8_t *p, size_t n);
uint32_t image_crc32(const uint8_t *p, size_t n);
/* Replace this producer with camera capture later; buffer ownership stays TX. */
void image_generate(uint8_t out[IMAGE_BYTES]);
unsigned image_segment_length(unsigned index);
void image_encode(uint8_t out[IMAGE_WIRE_BYTES], uint8_t type, uint32_t frame,
                  uint16_t token, uint16_t index, const uint8_t *body, unsigned length);
bool image_decode(const uint8_t *wire, size_t size, image_packet_t *packet);
void image_metadata(uint8_t out[20], uint32_t crc);
bool image_metadata_valid(const uint8_t *body, unsigned length);
bool image_bit(const uint8_t *bitmap, unsigned index);

typedef struct {
    uint8_t *pixels;
    uint8_t bitmap[IMAGE_BITMAP_BYTES];
    uint32_t frame, crc, started, last_activity, stats[IMAGE_STATS_WORDS];
    uint16_t token;
    bool active, verified, committed, failed, initial_seen;
    /* Optional foreground timer; native protocol users may leave it unset. */
    uint32_t (*clock)(void *context);
    void *clock_context;
    measurement_t phases[ICR_COUNT];
} image_receiver_t;
void image_receiver_measure(image_receiver_t *s, unsigned phase, uint32_t elapsed);
void image_receiver_init(image_receiver_t *s, uint8_t pixels[IMAGE_BYTES], uint32_t frame);
/* Foreground only. Validated DATA copied into pixels, never DMA-owned storage.
 * True means reply is ready; caller transmits after the request-END guard. */
bool image_receiver_packet(image_receiver_t *s, const uint8_t *wire, size_t size,
                           uint32_t now, uint8_t reply[IMAGE_WIRE_BYTES]);
void image_receiver_tick(image_receiver_t *s, uint32_t now);

typedef struct {
    void *context;
    uint32_t (*now)(void *context);
    bool (*send)(void *context, const uint8_t *wire);
    /* One request/reply attempt. Must bound wait and honour both direction guards. */
    bool (*exchange)(void *context, const uint8_t *request, uint8_t *reply);
    /* Optional drain for asynchronous send; called before timing a batch end.
     * exchange also drains before changing direction. */
    bool (*flush)(void *context);
} image_link_t;
/* Actual TX state machine, also used by native loss simulations. */
bool image_transmit(image_link_t *link, const uint8_t pixels[IMAGE_BYTES], uint32_t frame,
                    uint32_t stats[IMAGE_STATS_WORDS]);
#endif
