#include "clock.h"
#include "radio.h"
#include "protocol.h"
#include "usb.h"
#include "system.h"
#include "experiment.h"
#include "radio_timing.h"
#include "nrf.h"
#include <stddef.h>
#if IMAGE_MODE
int image_rx_main(void);
int main(void) { return image_rx_main(); }
#else

static protocol_stats_t stats;
static uint32_t radio_errors, reports_skipped, rearm_failures;
static uint32_t usb_work_max_us, end_during_usb;
static uint64_t uptime_us, observation_us, first_valid_time, last_valid_time;
static uint32_t validation_max_us;
static uint32_t telemetry[PROTOCOL_TELEMETRY_WORDS], telemetry_mask;
static uint32_t gaps_usb, gaps_no_usb, valid_usb, valid_no_usb;
static volatile bool usb_work_active;
extern volatile bool usb_irq_active;

static void received(const uint8_t *packet, bool crc, uint32_t end)
{
    uint32_t begin = radio_time();
    if (crc && protocol_decode_telemetry(packet, telemetry, &telemetry_mask)) return;
    uint32_t valid_before = stats.valid, gaps_before = stats.gaps;
    bool had_sequence = stats.have_sequence;
    protocol_record(&stats, packet, crc);
    if (stats.valid != valid_before) {
        uint64_t now = radio_time64();
        last_valid_time = now - (uint32_t)((uint32_t)now - end);
        if (!had_sequence) first_valid_time = last_valid_time;
        observation_us = last_valid_time - first_valid_time;
    }
    if (usb_work_active || usb_irq_active) {
        gaps_usb += stats.gaps - gaps_before;
        valid_usb += stats.valid - valid_before;
    } else {
        gaps_no_usb += stats.gaps - gaps_before;
        valid_no_usb += stats.valid - valid_before;
    }
    uint32_t duration = radio_time() - begin;
    if (duration > validation_max_us) validation_max_us = duration;
}
/* Small bounded formatter: no stdio, heap, floating point, or per-packet log. */
static char line[4096];
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
    /* Snapshot only; formatting/division/USB runs with RADIO interrupt enabled.
     * DMA can finish while masked, but RX cannot restart until we unmask. */
    protocol_stats_t snapshot;
    radio_timing_stats_t timing;
    uint32_t words[PROTOCOL_TELEMETRY_WORDS], mask, verify_max, gu, gn, vu, vn;
    uint64_t observed, idle;
    uint32_t irq_mask = __get_PRIMASK();
    __disable_irq();
    snapshot = stats;
    timing = radio_timing_stats;
    for (unsigned i = 0; i < PROTOCOL_TELEMETRY_WORDS; ++i) words[i] = telemetry[i];
    mask = telemetry_mask; observed = observation_us;
    verify_max = validation_max_us; gu = gaps_usb; gn = gaps_no_usb; vu = valid_usb; vn = valid_no_usb;
    idle = snapshot.have_sequence ? radio_time64() - last_valid_time : 0;
    __set_PRIMASK(irq_mask);
    protocol_stats_t stats = snapshot;
    radio_timing_stats_t radio_timing_stats = timing;
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
    field(" observation_ms=", observed / 1000u);
    field(" idle_ms=", idle / 1000u);
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
    field(" payload_bytes=", APP_PAYLOAD_SIZE);
    field(" packet_bytes=", RADIO_PACKET_SIZE);
    field(" on_air_bytes=", RADIO_ON_AIR_SIZE);
    field(" tx_interval_us=", TX_INTERVAL_US);
    field(" run_tag=", RUN_ID);
    field(" observation_us=", observed);
    field(" validation_max_us=", verify_max);
    field(" gaps_usb=", gu);
    field(" gaps_no_usb=", gn);
    field(" valid_usb=", vu);
    field(" valid_no_usb=", vn);
    field(" tx_stats_mask=", mask);
    field(" tx_attempts=", words[TX_ATTEMPTS]);
    field(" tx_starts=", words[TX_STARTS]);
    field(" tx_completions=", words[TX_COMPLETIONS]);
    field(" tx_missed=", words[TX_MISSED]);
    field(" tx_delay_min_us=", words[TX_DELAY_MIN]);
    field(" tx_delay_mean_us=", words[TX_DELAY_MEAN]);
    field(" tx_delay_max_us=", words[TX_DELAY_MAX]);
    field(" tx_spacing_n=", words[TX_SPACING_N]);
    field(" tx_spacing_min_us=", words[TX_SPACING_MIN]);
    field(" tx_spacing_mean_us=", words[TX_SPACING_MEAN]);
    field(" tx_spacing_max_us=", words[TX_SPACING_MAX]);
    field(" tx_turn_n=", words[TX_TURN_N]);
    field(" tx_turn_min_us=", words[TX_TURN_MIN]);
    field(" tx_turn_mean_us=", words[TX_TURN_MEAN]);
    field(" tx_turn_max_us=", words[TX_TURN_MAX]);
    field(" tx_duration_us=", ((uint64_t)words[TX_DURATION_HI] << 32) | words[TX_DURATION_LO]);
    field(" tx_failures=", words[TX_FAILURES]);
    field(" tx_complete=", words[TX_COMPLETE]);
    append("\r\n");
    if (!usb_write_line(line)) ++reports_skipped;
}

int main(void)
{
    time_init();
    if (!clock_init()) platform_fault(2);
    if (!usb_init()) platform_fault(5);
    radio_timing_init();
    bool armed = radio_init() && radio_receive(received);
    if (!armed) { ++radio_errors; ++rearm_failures; }
    uint32_t last_report = time_us(), last_retry = last_report, last_tick = last_report;
    bool banner = false;
    uint32_t last_usb = last_report;
    for (;;) {
        (void)radio_time64(); /* Extend the capture clock even during long silence. */
        uint32_t tick = time_us();
        uptime_us += (uint32_t)(tick - last_tick);
        last_tick = tick;
        if (armed && !radio_rx_healthy()) {
            (void)radio_disable(); armed = false; ++radio_errors;
        }
        if (!armed && (uint32_t)(tick - last_retry) >= 100000u) {
            last_retry = tick;
            armed = clock_init() && radio_init() && radio_receive(received);
            if (!armed) { ++radio_errors; ++rearm_failures; }
        }
        if ((uint32_t)(tick - last_usb) < 1000u) { __WFI(); continue; }
        last_usb = tick;
        uint32_t usb_start = time_us(), events = radio_rx_events;
        usb_work_active = true;
        usb_task();
        if (!usb_connected()) banner = false;
        else if (!banner) banner = usb_write_line("nRF52840 imaging M2 RX: 2Mbps CRC16 PPI_1MHz\r\n");
        if ((uint32_t)(tick - last_report) >= 1000000u) {
            last_report = tick;
            if (banner) report(armed);
        }
        usb_work_active = false;
        end_during_usb += radio_rx_events - events;
        uint32_t duration = time_us() - usb_start;
        if (duration > usb_work_max_us) usb_work_max_us = duration;
    }
}
#endif
