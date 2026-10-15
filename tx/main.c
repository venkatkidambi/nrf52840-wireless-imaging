#include "clock.h"
#include "radio.h"
#include "protocol.h"
#include "system.h"
#include "experiment.h"
#include "nrf.h"

/* Volatile counters are available through SWD without introducing TX USB load. */
volatile uint32_t tx_attempts, tx_failures, tx_recovery_failures;
volatile uint32_t tx_complete;

int main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    if (!radio_init()) platform_fault(3);
    uint32_t sequence = 0, last = time_us() - TX_INTERVAL_US;
    uint8_t packet[PROTOCOL_PAYLOAD_SIZE];
    _Static_assert(PROTOCOL_PAYLOAD_SIZE == RADIO_PAYLOAD_SIZE, "Payload mismatch");
    for (;;) {
#if TX_PACKET_COUNT != 0
        if (tx_attempts >= TX_PACKET_COUNT) {
            tx_complete = 1;
            for (;;) __WFE(); /* Reset TX to begin a new bounded run. */
        }
#endif
        uint32_t now = time_us();
        if ((uint32_t)(now - last) < TX_INTERVAL_US) continue;
        last = now; /* Attempt-start to attempt-start; no catch-up bursts. */
        protocol_encode(packet, sequence++);
        ++tx_attempts;
        if (!radio_transmit(packet)) {
            ++tx_failures;
            if (!clock_init() || !radio_init()) ++tx_recovery_failures;
        }
    }
}
