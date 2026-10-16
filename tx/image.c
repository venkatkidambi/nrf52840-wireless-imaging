#include "experiment.h"
#if IMAGE_MODE
#include "image_protocol.h"
#include "image_radio.h"
#include "clock.h"
#include "radio.h"
#include "radio_timing.h"
#include "system.h"
#include "nrf.h"
static uint8_t framebuffer[IMAGE_BYTES];
volatile uint32_t tx_complete, tx_failures;
static uint32_t stats[IMAGE_STATS_WORDS];
static uint32_t now(void *context) { (void)context; return radio_time(); }
static bool send(void *context, const uint8_t *wire)
{
    (void)context;
    bool ok = image_radio_send(wire);
    image_radio_stats(stats);
    return ok;
}
static bool exchange(void *context, const uint8_t *wire, uint8_t *reply)
{
    bool ok = image_radio_exchange(context, wire, reply);
    image_radio_stats(stats);
    return ok;
}
static bool flush(void *context)
{
    (void)context;
    bool ok = image_radio_flush();
    image_radio_stats(stats);
    return ok;
}
int image_tx_main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    radio_timing_init();
    if (!radio_init()) platform_fault(3);
    uint32_t start = radio_time();
    image_generate(framebuffer);
    stats[IM_GENERATION] = radio_time() - start;
    image_link_t link = {NULL, now, send, exchange, flush};
    tx_complete = image_transmit(&link, framebuffer, RUN_ID, stats);
    tx_failures = !tx_complete;
    (void)radio_disable();
    time_wakeup_stop();
    for (;;) __WFE();
}
#endif
