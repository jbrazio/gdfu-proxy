#pragma once

#include <stdint.h>

class StandbyButton {
  public:
    explicit StandbyButton(uint32_t holdMs) : holdMs_(holdMs) {}

    bool update(bool pressed, uint32_t now) {
        if (!pressed) {
            pressed_ = false;
            return false;
        }
        if (!pressed_) {
            pressed_ = true;
            pressedAtMs_ = now;
            return false;
        }
        return now - pressedAtMs_ >= holdMs_;
    }

  private:
    uint32_t holdMs_;
    bool pressed_ = false;
    uint32_t pressedAtMs_ = 0;
};
