#include "clock.h"
#include "radio.h"
#include "protocol.h"
#include "usb.h"
#include "system.h"
#include "experiment.h"
#include "radio_timing.h"
#include "nrf.h"
#include <stddef.h>

static protocol_stats_t stats;
static uint32_t radio_errors, reports_skipped, rearm_failures;
static uint32_t usb_work_max_us, end_during_usb;
static uint64_t uptime_us, first_valid_us, last_valid_us;

/* Small bounded formatter: no stdio, heap, floating point, or per-packet log. */
static char line[1536];
static size_t used;
static void append(const char *s)
{
    while (*s && used + 1 < sizeof line) line[used++] = *s++;
    line[used] = 0;
}
static void field(const char *name, uint64_t value)
{
    char digits[21];
    unsigned n = 0;
    append(name);
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n && used + 1 < sizeof line) line[used++] = digits[--n];
    line[used] = 0;
}
static void report(bool armed)
{
    used = 0;
    field("RX valid=", stats.valid);
    field(" radio_ready=", armed ? 1u : 0u);
    field(" crc_fail=", stats.crc_errors);
    field(" payload_fail=", stats.payload_errors);
    field(" gaps=", stats.gaps);
    field(" duplicates=", stats.duplicates);
    field(" backwards=", stats.backwards);
    field(" last=", stats.last_sequence);
    field(" radio_errors=", radio_errors);
    field(" usb_errors=", usb_errors);
    field(" reports_skipped=", reports_skipped);
    field(" freq_mhz=", RADIO_FREQ_MHZ);
    field(" tx_interval_ms=", TX_INTERVAL_MS);
    field(" tx_power_dbm=", RADIO_POWER_DBM);
    field(" packet_target=", TX_PACKET_COUNT);
    field(" rearm_failures=", rearm_failures);
    field(" have_sequence=", stats.have_sequence);
    field(" first=", stats.first_sequence);
    field(" observed_expected=", stats.observed_expected);
    field(" observed_gaps=", stats.observed_gaps);
    bool rate_valid = stats.have_sequence && !stats.backwards;
    field(" gap_rate_valid=", rate_valid);
    field(" gap_rate_ppm=", protocol_gap_ppm(&stats));
    field(" uptime_ms=", uptime_us / 1000u);
    field(" observation_ms=", (last_valid_us - first_valid_us) / 1000u);
    field(" idle_ms=", stats.have_sequence ? (uptime_us - last_valid_us) / 1000u : 0);
    field(" timing_n=", radio_timing_stats.rearm.count);
    field(" rearm_min_us=", radio_timing_stats.rearm.min);
    field(" rearm_mean_us=", measurement_mean(&radio_timing_stats.rearm));
    field(" rearm_max_us=", radio_timing_stats.rearm.max);
    field(" poll_min_us=", radio_timing_stats.poll.min);
    field(" poll_mean_us=", measurement_mean(&radio_timing_stats.poll));
    field(" poll_max_us=", radio_timing_stats.poll.max);
    field(" software_min_us=", radio_timing_stats.software.min);
    field(" software_mean_us=", measurement_mean(&radio_timing_stats.software));
    field(" software_max_us=", radio_timing_stats.software.max);
    field(" ramp_min_us=", radio_timing_stats.ramp.min);
    field(" ramp_mean_us=", measurement_mean(&radio_timing_stats.ramp));
    field(" ramp_max_us=", radio_timing_stats.ramp.max);
    field(" timing_rejected=", radio_timing_stats.rejected);
    field(" usb_work_max_us=", usb_work_max_us);
    field(" end_during_usb=", end_during_usb);
    field(" usb_irq_max_us=", usb_irq_max_us);
    field(" usb_end_in_irq=", usb_end_in_irq);
    append("\r\n");
    if (!usb_write_line(line)) ++reports_skipped;
}

int main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    if (!usb_init()) platform_fault(5);
    radio_timing_init();
    bool initialized = radio_init();
    bool armed = initialized && radio_receive();
    if (initialized && !armed) ++rearm_failures;
    if (!armed) ++radio_errors;
    uint32_t last_report = time_us(), last_retry = last_report;
    uint32_t last_tick = last_report;
    bool banner = false;
    uint8_t packet[PROTOCOL_PAYLOAD_SIZE];
    _Static_assert(PROTOCOL_PAYLOAD_SIZE == RADIO_PAYLOAD_SIZE, "Payload mismatch");
    for (;;) {
        uint32_t tick = time_us();
        uptime_us += (uint32_t)(tick - last_tick);
        last_tick = tick;
        if (armed) {
            bool crc_ok = false;
            radio_result_t result = radio_poll(packet, &crc_ok);
            if (result != RADIO_IDLE) {
                /* Restore reception before validation, formatting, or USB work. */
                armed = radio_receive();
                if (!armed) ++rearm_failures;
                if (result == RADIO_ERROR || !armed) ++radio_errors;
                if (result == RADIO_PACKET) {
                    uint32_t prior_valid = stats.valid;
                    bool had_sequence = stats.have_sequence;
                    protocol_record(&stats, packet, crc_ok);
                    if (stats.valid != prior_valid) {
                        last_valid_us = uptime_us + (uint32_t)(time_us() - last_tick);
                        if (!had_sequence) first_valid_us = last_valid_us;
                    }
                }
            }
        }
        uint32_t now = time_us();
        if (!armed && (uint32_t)(now - last_retry) >= 100000u) {
            last_retry = now;
            initialized = clock_init() && radio_init();
            armed = initialized && radio_receive();
            if (initialized && !armed) ++rearm_failures;
            if (!armed) ++radio_errors;
        }
        /* Poll again before USB if a packet finished during validation/recovery.
         * USB remains interrupt-driven plus nonblocking foreground service. */
        if (armed && NRF_RADIO->EVENTS_END) continue;
        uint32_t usb_start = time_us();
        bool ended_before_usb = NRF_RADIO->EVENTS_END != 0;
        usb_task();
        if (!usb_connected()) banner = false;
        else if (!banner) {
            banner = usb_write_line("nRF52840 imaging M1.1 RX: 2Mbps payload=32 CRC16 "
                                    EXP_CONFIG_TEXT " timing=PPI_1MHz\r\n");
        }
        if ((uint32_t)(now - last_report) >= 1000000u) {
            last_report = now;
            tick = time_us();
            uptime_us += (uint32_t)(tick - last_tick);
            last_tick = tick;
            if (banner) report(armed);
        }
        if (!ended_before_usb && NRF_RADIO->EVENTS_END) ++end_during_usb;
        uint32_t usb_duration = time_us() - usb_start;
        if (usb_duration > usb_work_max_us) usb_work_max_us = usb_duration;
    }
}
