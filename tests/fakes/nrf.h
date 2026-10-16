/* Host register model for common/clock.c only, not a device emulator.
 * Numeric masks match the pinned nRF52840 MDK; no physical timing is inferred. */
#ifndef TEST_CLOCK_NRF_H
#define TEST_CLOCK_NRF_H
#include <stdint.h>
typedef struct {
    uint32_t TASKS_STOP, TASKS_START, TASKS_CLEAR, TASKS_CAPTURE[4];
    uint32_t INTENCLR, INTENSET, SHORTS, MODE, BITMODE, PRESCALER;
    uint32_t CC[4], EVENTS_COMPARE[4];
} test_timer_t;
typedef struct { uint32_t HFCLKSTAT, EVENTS_HFCLKSTARTED, TASKS_HFCLKSTART; } test_clock_t;
extern test_timer_t test_timer0, test_timer1;
extern test_clock_t test_clock;
extern unsigned test_ipsr, test_irq_enabled, test_irq_pending, test_irq_priority;
#define NRF_TIMER0 (&test_timer0)
#define NRF_TIMER1 (&test_timer1)
#define NRF_CLOCK (&test_clock)
#define TIMER_MODE_MODE_Timer 0u
#define TIMER_BITMODE_BITMODE_32Bit 3u
#define TIMER_SHORTS_COMPARE0_CLEAR_Msk 1u
#define TIMER_INTENSET_COMPARE0_Msk (1u << 16)
#define CLOCK_HFCLKSTAT_STATE_Pos 16u
#define CLOCK_HFCLKSTAT_STATE_Msk (1u << 16)
#define CLOCK_HFCLKSTAT_SRC_Pos 0u
#define CLOCK_HFCLKSTAT_SRC_Msk 1u
#define CLOCK_HFCLKSTAT_STATE_Running 1u
#define CLOCK_HFCLKSTAT_SRC_Xtal 1u
#define RADIO_IRQn 1u
#define TIMER1_IRQn 9u
static inline unsigned __get_IPSR(void) { return test_ipsr; }
static inline void __DSB(void) {}
void NVIC_DisableIRQ(unsigned irq);
void NVIC_EnableIRQ(unsigned irq);
void NVIC_ClearPendingIRQ(unsigned irq);
void NVIC_SetPriority(unsigned irq, unsigned priority);
#endif
