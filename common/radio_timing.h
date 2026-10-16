#ifndef IMAGING_RADIO_TIMING_H
#define IMAGING_RADIO_TIMING_H
#include "measure.h"
typedef struct {
    measurement_t rearm, poll, software, ramp;
    uint32_t rejected;
} radio_timing_stats_t;
extern radio_timing_stats_t radio_timing_stats;
void radio_timing_init(void);
uint32_t radio_time(void);
uint64_t radio_time64(void);
uint32_t radio_end_time(void);
uint32_t radio_address_time(void);
uint32_t radio_start_time(void);
void radio_start_now(void);
/* Returns false when the deadline is too close/past; rebases to a safe future compare then. */
bool radio_start_at(uint32_t deadline);
void radio_timing_cancel(void);
void radio_timing_rearm(uint32_t end, uint32_t handled);
#endif
