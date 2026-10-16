#include "image_usb.h"
#include <string.h>
void image_usb_header(uint8_t out[IMAGE_USB_HEADER], uint8_t type, uint32_t frame,
                      const uint8_t *body, uint32_t length)
{
    memcpy(out, "NRFIMG3\n", 8);
    out[8] = 1; out[9] = type; out[10] = out[11] = 0;
    image_put32(out + 12, frame); image_put32(out + 16, length);
    uint32_t crc = image_crc_update(~0u, out, 20);
    image_put32(out + 20, ~image_crc_update(crc, body, length));
}
#include "experiment.h"
#if IMAGE_MODE
#include "usb.h"
#include "clock.h"
#include "image_radio.h"
static uint8_t header[64], body[IMAGE_STATS_WORDS * 4u];
static const uint8_t *payload;
static uint32_t length, offset, last_ready, usb_start, prefix_length;
static uint8_t sending;
static bool connected, start_sent, pixels_sent, result_sent;
static void begin(uint8_t type, image_receiver_t *s, const uint8_t *p, uint32_t n)
{
    uint32_t began = time_us();
    image_usb_header(header, type, s->frame, p, n);
    if (type == IMAGE_USB_PIXELS) s->stats[IM_USB_CRC_US] = time_us() - began;
    uint32_t first = n < sizeof header - IMAGE_USB_HEADER ? n : sizeof header - IMAGE_USB_HEADER;
    memcpy(header + IMAGE_USB_HEADER, p, first);
    prefix_length = IMAGE_USB_HEADER + first;
    payload = p; length = n; offset = 0; sending = type;
}
void image_usb_task(image_receiver_t *s, bool radio_ready)
{
    usb_task();
    if (!usb_connected()) {
        if (connected) usb_clear_output();
        connected = false; sending = 0;
        start_sent = pixels_sent = result_sent = false;
        return;
    }
    bool just_connected = !connected;
    connected = true;
    if (!sending) {
        if (!s->active && !s->failed && (just_connected || (uint32_t)(time_us() - last_ready) >= 1000000u)) {
            const uint32_t config[] = {RADIO_FREQ_MHZ, TX_INTERVAL_MS, TX_PACKET_COUNT,
                RADIO_POWER_DBM, APP_PAYLOAD_SIZE, TX_INTERVAL_US, RUN_ID, radio_ready,
                s->active, image_radio_crc_errors, s->stats[IM_INVALID], image_radio_drops, usb_errors,
                IMAGE_CONTROL_PROFILE};
            for (unsigned i = 0; i < sizeof config / sizeof config[0]; ++i) image_put32(body + 4*i, config[i]);
            begin(IMAGE_USB_READY, s, body, sizeof config); last_ready = time_us();
        } else if (s->failed && !result_sent) {
            image_put32(body, s->stats[IM_STATUS]);
            begin(IMAGE_USB_ERROR, s, body, 4);
        } else if (s->active && !start_sent) {
            image_metadata(body, s->crc); begin(IMAGE_USB_START, s, body, 20);
        } else if (s->committed && !pixels_sent) {
            usb_start = time_us();
            begin(IMAGE_USB_PIXELS, s, s->pixels, IMAGE_BYTES);
        } else if (s->committed && pixels_sent && !result_sent) {
            for (unsigned i = 0; i < IMAGE_STATS_WORDS; ++i) image_put32(body + 4*i, s->stats[i]);
            begin(IMAGE_USB_RESULT, s, body, sizeof body);
        }
    }
    if (!sending) return;
    if (offset < prefix_length) {
        /* A full first USB packet avoids flushing a 24-byte short header. */
        offset += usb_write_bytes(header + offset, prefix_length - offset);
    } else if (offset < IMAGE_USB_HEADER + length) {
        uint32_t pos = offset - IMAGE_USB_HEADER;
        uint32_t n = length - pos;
        if (n > 512) n = 512; /* Bound each foreground visit, preserving radio service. */
        offset += usb_write_bytes(payload + pos, n);
    }
    if (offset != IMAGE_USB_HEADER + length) return;
    if (sending == IMAGE_USB_START) start_sent = true;
    if (sending == IMAGE_USB_PIXELS) {
        pixels_sent = true;
        s->stats[IM_USB_US] = time_us() - usb_start;
    }
    if (sending == IMAGE_USB_RESULT || sending == IMAGE_USB_ERROR) result_sent = true;
    sending = 0;
}
#endif
