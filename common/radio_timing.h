#ifndef IMAGING_RADIO_TIMING_H
#define IMAGING_RADIO_TIMING_H
#include "measure.h"
typedef struct {
    measurement_t rearm, poll, software, ramp;
    uint32_t rejected;
} radio_timing_stats_t;
extern radio_timing_stats_t radio_timing_stats;
void radio_timing_init(void);
void radio_timing_end(void);
void radio_timing_rxen(void);
void radio_timing_ready(void);
void radio_timing_cancel(void);
#endif
