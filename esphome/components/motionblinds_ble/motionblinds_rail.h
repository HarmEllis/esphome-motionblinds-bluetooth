#pragma once

#include <cstdint>

namespace esphome::motionblinds_ble {

/* Translation between one motor's own frame of reference and the window.
 *
 * Raw protocol positions run 0 (fully open) to 100 (fully closed) in whatever
 * orientation the motor happens to be mounted, and a motor may be calibrated
 * over only part of the window. Every piece of geometry in this project works
 * in window coordinates instead — 0 at the top of the window, 100 at the
 * bottom — so this transform sits on the boundary and nothing downstream has
 * to think about mounting again.
 *
 * Lives here rather than with the top-down bottom-up geometry because it is a
 * property of a single motor, and because the coordinator depends on the motor
 * component and not the other way round.
 */
struct RailRange {
  float window_min{0.0f};
  float window_max{100.0f};
  bool invert{false};

  float to_window(float raw) const {
    const float span = this->window_max - this->window_min;
    return this->invert ? this->window_max - raw * span / 100.0f : this->window_min + raw * span / 100.0f;
  }

  float to_raw(float window) const {
    const float span = this->window_max - this->window_min;
    if (span <= 0.0f)
      return 0.0f;
    const float raw =
        this->invert ? (this->window_max - window) * 100.0f / span : (window - this->window_min) * 100.0f / span;
    return raw < 0.0f ? 0.0f : (raw > 100.0f ? 100.0f : raw);
  }

  bool valid() const {
    return this->window_min >= 0.0f && this->window_max <= 100.0f && this->window_min < this->window_max;
  }
};

}  // namespace esphome::motionblinds_ble
