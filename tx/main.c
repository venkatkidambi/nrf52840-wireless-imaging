#include "clock.h"
#include "radio.h"
#include "protocol.h"
#include "system.h"
#include "radio_timing.h"
#include "nrf.h"
#if IMAGE_MODE
int image_tx_main(void);
int main(void) { return image_tx_main(); }
#else

volatile uint32_t tx_attempts, tx_failures, tx_complete;
static struct { uint8_t bytes[PROTOCOL_PAYLOAD_SIZE]; } __attribute__((aligned(4))) packets[2];
static uint32_t final[PROTOCOL_TELEMETRY_WORDS];

int main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    radio_timing_init();
    if (!radio_init() || !radio_tx_enable()) platform_fault(3);
    measurement_t spacing = {0}, turnaround = {0}, delay = {0};
    radio_tx_result_t prior = {0}, result = {0};
    uint64_t duration = 0;
    uint32_t starts = 0, completions = 0, missed = 0;
    const uint32_t target = TX_PACKET_COUNT ? TX_PACKET_COUNT : PROTOCOL_DATA_LIMIT;
    unsigned active = 0;
    protocol_encode(packets[active].bytes, 0);
    while (tx_attempts < target) {
        bool scheduled = TX_INTERVAL_US && tx_attempts;
        uint32_t deadline = prior.start + TX_INTERVAL_US;
        bool on_time = radio_tx_submit(packets[active].bytes, scheduled, deadline);
        if (scheduled && !on_time) ++missed;
        ++tx_attempts;
        /* Prepare only the OTHER SRAM buffer while RADIO owns active. */
        if (tx_attempts < target) protocol_encode(packets[active ^ 1u].bytes, tx_attempts);
        if (!radio_tx_wait(&result)) {
            if (result.addressed) ++starts;
            ++tx_failures; break;
        }
        ++starts; /* ADDRESS proves the scheduled START reached the transmitter. */
        ++completions;
        if (completions > 1) {
            measurement_add(&spacing, result.address - prior.address);
            measurement_add(&turnaround, result.start - prior.end);
            duration += (uint32_t)(result.end - prior.end);
        } else {
            /* ADDRESS marks the end of the fixed 1+5 byte preamble/address. */
            duration = (uint32_t)(result.end - result.address) + 24u;
        }
        if (scheduled) {
            int32_t late = (int32_t)(result.start - deadline);
            measurement_add(&delay, late > 0 ? (uint32_t)late : 0);
        }
        prior = result;
        active ^= 1u;
    }
    tx_complete = completions == target;
    final[TX_ATTEMPTS] = tx_attempts;
    final[TX_STARTS] = starts;
    final[TX_COMPLETIONS] = completions;
    final[TX_MISSED] = missed;
    final[TX_DELAY_MIN] = delay.min; final[TX_DELAY_MEAN] = measurement_mean(&delay);
    final[TX_DELAY_MAX] = delay.max;
    final[TX_SPACING_N] = spacing.count; final[TX_SPACING_MIN] = spacing.min;
    final[TX_SPACING_MEAN] = measurement_mean(&spacing); final[TX_SPACING_MAX] = spacing.max;
    final[TX_TURN_N] = turnaround.count; final[TX_TURN_MIN] = turnaround.min;
    final[TX_TURN_MEAN] = measurement_mean(&turnaround); final[TX_TURN_MAX] = turnaround.max;
    final[TX_DURATION_LO] = (uint32_t)duration; final[TX_DURATION_HI] = (uint32_t)(duration >> 32);
    final[TX_FAILURES] = tx_failures; final[TX_COMPLETE] = tx_complete;
    /* Three unacknowledged final-stat pages, outside the timed data run.
     * Sent once each at 10 ms spacing; loss leaves an incomplete telemetry mask. */
    if (radio_disable() && radio_tx_enable()) {
        uint32_t deadline = radio_time() + 10000u;
        for (unsigned i = 0; i < 3; ++i) {
            protocol_encode_telemetry(packets[0].bytes, final, i);
            (void)radio_tx_submit(packets[0].bytes, true, deadline);
            if (!radio_tx_wait(&result)) break;
            deadline = result.start + 10000u;
        }
    }
    (void)radio_disable();
    time_wakeup_stop();
    for (;;) __WFE();
}
#endif
