#ifndef IMAGING_MEASURE_H
#define IMAGING_MEASURE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint64_t sum;
    uint32_t count, min, max;
} measurement_t;
/* Saturate sample count, never silently overflow the sum or divisor. */
static inline void measurement_add(measurement_t *s, uint32_t value)
{
    if (s->count == UINT32_MAX) return;
    if (!s->count || value < s->min) s->min = value;
    if (value > s->max) s->max = value;
    s->sum += value;
    ++s->count;
}
static inline uint32_t measurement_mean(const measurement_t *s)
{
    return s->count ? (uint32_t)(s->sum / s->count) : 0;
}
/* A complete sample must be ordered and shorter than one second. Modular
 * subtraction permits a timer wrap inside the sample. Debug halts invalidate it. */
static inline bool measurement_spans(uint32_t end, uint32_t poll, uint32_t rxen,
                                     uint32_t ready, uint32_t spans[4])
{
    spans[0] = ready - end;
    spans[1] = poll - end;
    spans[2] = rxen - poll;
    spans[3] = ready - rxen;
    return spans[0] < 1000000u &&
        (uint64_t)spans[1] + spans[2] + spans[3] == spans[0];
}
#endif
