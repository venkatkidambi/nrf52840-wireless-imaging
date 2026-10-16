/* Behavioral register model, not a timing or RF simulator. */
#ifndef TEST_RADIO_NRF_H
#define TEST_RADIO_NRF_H
#include <stdint.h>
typedef struct {
    uint32_t INTENCLR, INTENSET, SHORTS, STATE, EVENTS_DISABLED, TASKS_DISABLE;
    uint32_t POWER, MODE, MODECNF0, FREQUENCY, TXPOWER, PCNF0, PCNF1;
    uint32_t BASE0, PREFIX0, TXADDRESS, RXADDRESSES, CRCCNF, CRCPOLY, CRCINIT;
    uint32_t EVENTS_END, EVENTS_ADDRESS, EVENTS_CRCOK, EVENTS_CRCERROR;
    uint32_t EVENTS_READY, TASKS_RXEN, TASKS_TXEN, PACKETPTR, CRCSTATUS, TASKS_START;
} fake_radio_t;
typedef struct {
    uint32_t TASKS_STOP, TASKS_START, TASKS_CLEAR, TASKS_CAPTURE[6];
    uint32_t INTENCLR, SHORTS, MODE, BITMODE, PRESCALER, CC[6], EVENTS_COMPARE[6];
} fake_timer_t;
typedef struct {
    uint32_t CHENCLR, CHENSET, CHG[1];
    struct { uint32_t EEP, TEP; } CH[5];
    struct { uint32_t TEP; } FORK[5];
    struct { uint32_t DIS, EN; } TASKS_CHG[1];
} fake_ppi_t;
typedef struct { uint32_t INTENCLR, EVENTS_TRIGGERED[1], TASKS_TRIGGER[1]; } fake_egu_t;
extern fake_radio_t fake_radio;
extern fake_timer_t fake_timer;
extern fake_ppi_t fake_ppi;
extern fake_egu_t fake_egu;
extern uint32_t fake_mask;
#define NRF_RADIO (&fake_radio)
#define NRF_TIMER3 (&fake_timer)
#define NRF_PPI (&fake_ppi)
#define NRF_EGU0 (&fake_egu)
#define RADIO_IRQn 1u
#define RADIO_STATE_STATE_Disabled 0u
#define RADIO_STATE_STATE_RxIdle 2u
#define RADIO_STATE_STATE_Rx 3u
#define RADIO_STATE_STATE_TxIdle 10u
#define RADIO_STATE_STATE_Tx 11u
#define RADIO_MODE_MODE_Nrf_2Mbit 1u
#define RADIO_MODECNF0_RU_Fast 1u
#define RADIO_MODECNF0_RU_Pos 0u
#define RADIO_FREQUENCY_MAP_Low 1u
#define RADIO_FREQUENCY_MAP_Default 0u
#define RADIO_FREQUENCY_MAP_Pos 8u
#define RADIO_FREQUENCY_FREQUENCY_Pos 0u
#define RADIO_TXPOWER_TXPOWER_0dBm 0u
#define RADIO_PCNF0_PLEN_8bit 0u
#define RADIO_PCNF0_PLEN_Pos 24u
#define RADIO_PCNF1_MAXLEN_Pos 0u
#define RADIO_PCNF1_STATLEN_Pos 8u
#define RADIO_PCNF1_BALEN_Pos 16u
#define RADIO_PCNF1_ENDIAN_Little 0u
#define RADIO_PCNF1_ENDIAN_Pos 24u
#define RADIO_PCNF1_WHITEEN_Disabled 0u
#define RADIO_PCNF1_WHITEEN_Pos 25u
#define RADIO_CRCCNF_LEN_Two 2u
#define RADIO_CRCCNF_LEN_Pos 0u
#define RADIO_CRCCNF_SKIPADDR_Include 0u
#define RADIO_CRCCNF_SKIPADDR_Pos 8u
#define RADIO_INTENSET_END_Msk (1u << 3)
#define RADIO_CRCSTATUS_CRCSTATUS_CRCOk 1u
#define TIMER_MODE_MODE_Timer 0u
#define TIMER_BITMODE_BITMODE_32Bit 3u
static inline uint32_t __get_PRIMASK(void) { return fake_mask; }
static inline void __disable_irq(void) { fake_mask = 1; }
static inline void __set_PRIMASK(uint32_t m) { fake_mask = m; }
static inline void __DSB(void) {}
static inline void __DMB(void) {}
void __WFI(void);
void NVIC_DisableIRQ(unsigned irq);
void NVIC_EnableIRQ(unsigned irq);
void NVIC_ClearPendingIRQ(unsigned irq);
void NVIC_SetPriority(unsigned irq, unsigned priority);
#endif
