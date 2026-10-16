#include "experiment.h"
#include "image_control.h"

/* Retained build metadata, with no runtime work or protocol changes. */
__attribute__((used, section(".experiment_identity")))
const struct {
    char magic[16];
    uint32_t frequency, interval_ms, target, power, payload, interval_us, run_id;
#if IMAGE_MODE
    uint32_t control_profile;
#endif
} experiment_identity = {
#if IMAGE_MODE
    "IMAGING-CONFIG4",
#else
    "IMAGING-CONFIG2",
#endif
    RADIO_FREQ_MHZ, TX_INTERVAL_MS,
    TX_PACKET_COUNT, RADIO_POWER_DBM, APP_PAYLOAD_SIZE, TX_INTERVAL_US, RUN_ID
#if IMAGE_MODE
    , IMAGE_CONTROL_PROFILE
#endif
};
