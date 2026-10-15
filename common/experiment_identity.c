#include "experiment.h"

/* Retained build metadata, with no runtime work or protocol changes. */
__attribute__((used, section(".experiment_identity")))
const struct {
    char magic[16];
    uint32_t frequency, interval, target, power;
} experiment_identity = {
    "IMAGING-CONFIG1", RADIO_FREQ_MHZ, TX_INTERVAL_MS,
    TX_PACKET_COUNT, RADIO_POWER_DBM
};
