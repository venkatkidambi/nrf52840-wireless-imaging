#ifndef IMAGING_RADIO_H
#define IMAGING_RADIO_H
#include <stdbool.h>
#include <stdint.h>
#define RADIO_PAYLOAD_SIZE 32u
typedef enum { RADIO_IDLE, RADIO_PACKET, RADIO_ERROR } radio_result_t;
bool radio_init(void);
bool radio_disable(void);
bool radio_transmit(const uint8_t packet[RADIO_PAYLOAD_SIZE]);
/* Start asynchronous RX. poll copies a complete packet; caller must rearm. */
bool radio_receive(void);
radio_result_t radio_poll(uint8_t packet[RADIO_PAYLOAD_SIZE], bool *crc_ok);
#endif
