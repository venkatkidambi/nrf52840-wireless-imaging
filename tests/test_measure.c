#include "measure.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(measurement_deadline_safe(100, 108));
    assert(!measurement_deadline_safe(100, 107));
    assert(!measurement_deadline_safe(100, 99));
    assert(measurement_deadline_safe(0xfffffffc, 4));
    assert(!measurement_deadline_safe(4, 0xfffffffc));
    measurement_t s = {0};
    assert(measurement_mean(&s) == 0);
    measurement_add(&s, 140);
    measurement_add(&s, 150);
    measurement_add(&s, 160);
    assert(s.count == 3 && s.min == 140 && s.max == 160);
    assert(measurement_mean(&s) == 150);
    s = (measurement_t){0};
    measurement_add(&s, UINT32_MAX);
    measurement_add(&s, UINT32_MAX);
    assert(s.sum == 2ull * UINT32_MAX && measurement_mean(&s) == UINT32_MAX);
    s.count = UINT32_MAX - 1u;
    s.sum = (uint64_t)s.count * UINT32_MAX;
    measurement_add(&s, UINT32_MAX);
    assert(s.sum == (uint64_t)UINT32_MAX * UINT32_MAX);
    measurement_add(&s, 0); /* Saturation preserves all fields and mean. */
    assert(s.count == UINT32_MAX && s.min == UINT32_MAX);
    assert(measurement_mean(&s) == UINT32_MAX);
    uint32_t spans[4];
    assert(measurement_spans(UINT32_MAX - 9, 0, 10, 150, spans));
    assert(spans[0] == 160 && spans[1] == 10 && spans[2] == 10 && spans[3] == 140);
    assert(!measurement_spans(0, 100, 50, 200, spans));
    assert(!measurement_spans(0, 1, 2, 1000000, spans));
    assert(measurement_spans(100, 100, 101, 241, spans));
    puts("Timing: wrap, ordered spans, rejection, 64-bit sums, saturation passed");
}
