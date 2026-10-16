#include "radio_timing.h"
#include "nrf.h"
#include <assert.h>
#include <stdio.h>
fake_radio_t fake_radio;
fake_timer_t fake_timer;
fake_ppi_t fake_ppi;
fake_egu_t fake_egu;
uint32_t fake_mask;
int main(void)
{
    radio_timing_init();
    assert(fake_timer.PRESCALER == 4 && fake_ppi.CHG[0] == 24 && fake_ppi.CHENSET == 7);
    assert(fake_ppi.CH[3].TEP == (uint32_t)(uintptr_t)&fake_radio.TASKS_START);
    assert(fake_ppi.CH[4].TEP == (uint32_t)(uintptr_t)&fake_ppi.TASKS_CHG[0].DIS);
    assert(fake_ppi.FORK[3].TEP == (uint32_t)(uintptr_t)&fake_timer.TASKS_CAPTURE[3]);
    const uint32_t times[] = {0, 100, UINT32_MAX - 3};
    for (unsigned i = 0; i < 3; ++i) {
        fake_timer.CC[2] = times[i];
        assert(radio_start_at(times[i] + 8));
        assert(fake_timer.CC[4] == times[i] + 8);
        assert(!radio_start_at(times[i] + 7));
        assert(fake_timer.CC[4] == times[i] + 8);
        assert(!radio_start_at(times[i] - 1));
        assert(fake_timer.CC[4] == times[i] + 8);
        assert(fake_timer.EVENTS_COMPARE[4] == 0 && fake_ppi.TASKS_CHG[0].EN == 1);
    }
    radio_timing_cancel(); assert(fake_ppi.TASKS_CHG[0].DIS == 1);
    puts("TIMER/PPI: one-shot routing, safe-arm boundary, late deadlines and wrap passed");
}
