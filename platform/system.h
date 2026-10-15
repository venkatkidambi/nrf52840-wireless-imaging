#ifndef IMAGING_SYSTEM_H
#define IMAGING_SYSTEM_H
#include <stdint.h>
void platform_init(void);
__attribute__((noreturn)) void platform_fault(uint32_t code);
extern volatile uint32_t platform_fault_code;
#endif
