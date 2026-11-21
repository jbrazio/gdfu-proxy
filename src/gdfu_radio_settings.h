#ifndef GDFU_RADIO_SETTINGS_H
#define GDFU_RADIO_SETTINGS_H

#include <stdint.h>

#include "gdfu_config.h"

#define GDFU_DEFAULT_PROFILE GDFU_PROFILE_ID_433
#define GDFU_DEFAULT_TX_POWER_DBM 10u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t profile;
    uint8_t tx_power_dbm;
} gdfu_radio_settings_t;

gdfu_radio_settings_t gdfu_radio_settings_normalize(uint8_t profile, uint8_t tx_power_dbm);

#ifdef __cplusplus
}
#endif

#endif
