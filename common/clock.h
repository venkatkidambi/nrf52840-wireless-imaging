#ifndef IMAGING_CLOCK_H
#define IMAGING_CLOCK_H
#include <stdbool.h>
#include <stdint.h>
void time_init(void);
uint32_t time_us(void);
bool clock_hf_running(void);
bool clock_init(void);
bool wait_event(volatile uint32_t *event, uint32_t timeout_us);
#endif
