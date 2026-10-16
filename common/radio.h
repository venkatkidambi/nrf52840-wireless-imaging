#ifndef IMAGING_RADIO_H
#define IMAGING_RADIO_H
#include <stdbool.h>
#include <stdint.h>
#include "experiment.h"
#define RADIO_PAYLOAD_SIZE RADIO_PACKET_SIZE
/* Callback runs at RADIO IRQ priority 1, after the alternate DMA buffer starts.
 * Packet ownership lasts only until callback returns. Never retain the pointer. */
typedef void (*radio_rx_handler_t)(const uint8_t *packet, bool crc_ok, uint32_t end);
typedef struct {
    uint32_t start, address, end, deadline;
    bool addressed, scheduled, on_time;
} radio_tx_result_t;
bool radio_init(void);
bool radio_disable(void);
bool radio_receive(radio_rx_handler_t handler);
bool radio_rx_healthy(void);
bool radio_tx_enable(void);
/* Buffer must be aligned SRAM and immutable until wait returns. */
bool radio_tx_submit(const uint8_t *packet, bool scheduled, uint32_t deadline);
/* Stage ONE immutable alternate SRAM buffer before waiting for the current one.
 * END ISR hands it to DMA and arms a one-shot compare. At most two buffers may
 * be outstanding; each needs one wait, in submission order. interval=0 keeps
 * the 20 us RX handoff allowance and otherwise runs as fast as safe arming allows.
 * A late producer leaves TXIDLE; it never repeats a buffer or catches up. */
bool radio_tx_stage(const uint8_t *packet, uint32_t interval);
bool radio_tx_wait(radio_tx_result_t *result);
extern volatile uint32_t radio_rx_events;
#endif
