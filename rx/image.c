#include "experiment.h"
#if IMAGE_MODE
#include "image_protocol.h"
#include "image_radio.h"
#include "image_usb.h"
#include "clock.h"
#include "radio.h"
#include "radio_timing.h"
#include "usb.h"
#include "system.h"
#include "nrf.h"
static uint8_t framebuffer[IMAGE_BYTES];
static image_receiver_t receiver;
static uint8_t packet[IMAGE_WIRE_BYTES], reply[IMAGE_WIRE_BYTES];
int image_rx_main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    if (!usb_init()) platform_fault(5);
    radio_timing_init();
    image_receiver_init(&receiver, framebuffer, RUN_ID);
    bool armed = radio_init() && image_radio_listen();
    for (;;) {
        uint32_t end;
        if (armed && image_radio_pop(packet, &end)) {
            bool verifying = !receiver.verified && receiver.stats[IM_RECEIVED] == IMAGE_PACKETS && packet[1] == IMG_END;
            uint32_t began = radio_time();
            bool respond = image_receiver_packet(&receiver, packet, sizeof packet, end, reply);
            if (verifying) receiver.stats[IM_VERIFY_US] = radio_time() - began;
            if (respond) {
                /* Never send an overdue response into the peer's next retry.
                 * Its repeated request will produce the same reply. */
                if ((uint32_t)(radio_time() - end) < IMAGE_REPLY_TIMEOUT_US - 10000u) {
                    image_radio_guard(end);
                    armed = image_radio_send(reply) && image_radio_listen();
                }
            }
        }
        image_receiver_tick(&receiver, radio_time());
        if (armed && !radio_rx_healthy()) armed = false;
        if (!armed) {
            (void)radio_disable();
            receiver.failed = true; receiver.stats[IM_STATUS] = IMAGE_ERR_RADIO;
        }
        receiver.stats[IM_CRC_ERRORS] = image_radio_crc_errors;
        receiver.stats[IM_QUEUE_DROPS] = image_radio_drops;
        receiver.stats[IM_RX_TURN_MAX] = image_radio_turn_max;
        image_usb_task(&receiver, armed);
    }
}
#endif
