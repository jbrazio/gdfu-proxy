#pragma once

#include <stdint.h>

constexpr uint32_t OLED_IDLE_UPDATE_INTERVAL_MS = 100;
constexpr uint32_t OLED_DFU_UPDATE_INTERVAL_MS = 2000;

inline bool oledShouldUpdate(bool ready, bool force, bool streaming, bool dirty,
                             uint32_t elapsedMs) {
    if (!ready)
        return false;
    if (force)
        return true;
    if (!dirty)
        return false;
    return elapsedMs >= (streaming ? OLED_DFU_UPDATE_INTERVAL_MS : OLED_IDLE_UPDATE_INTERVAL_MS);
}
