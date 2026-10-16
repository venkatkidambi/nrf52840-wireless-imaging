#ifndef IMAGE_RADIO_H
#define IMAGE_RADIO_H
#include "image_protocol.h"
/* Foreground owns these copies; ISR owns queue slots until publication. */
bool image_radio_listen(void);
bool image_radio_pop(uint8_t out[IMAGE_WIRE_BYTES], uint32_t *end);
bool image_radio_send(const uint8_t *wire);
bool image_radio_flush(void);
void image_radio_stats(uint32_t stats[IMAGE_STATS_WORDS]);
void image_radio_rx_stats(uint32_t stats[IMAGE_STATS_WORDS]);
bool image_radio_exchange(void *context, const uint8_t *request, uint8_t *reply);
void image_radio_guard(uint32_t end);
extern uint32_t image_radio_turn_max;
extern volatile uint32_t image_radio_crc_errors, image_radio_drops;
extern measurement_t image_radio_confirmation, image_radio_terminal_guard;
#endif
