#include "gdfu_radio_settings.h"

static int profile_valid(uint8_t profile) {
    return profile == GDFU_PROFILE_ID_433 || profile == GDFU_PROFILE_ID_868;
}

static int tx_power_valid(uint8_t tx_power_dbm) {
    return tx_power_dbm >= GDFU_MIN_TX_POWER_DBM && tx_power_dbm <= GDFU_MAX_TX_POWER_DBM;
}

gdfu_radio_settings_t gdfu_radio_settings_normalize(uint8_t profile, uint8_t tx_power_dbm) {
    gdfu_radio_settings_t settings = {
        profile_valid(profile) ? profile : GDFU_PROFILE_ID_433,
        tx_power_valid(tx_power_dbm) ? tx_power_dbm : GDFU_DEFAULT_TX_POWER_DBM,
    };
    return settings;
}
