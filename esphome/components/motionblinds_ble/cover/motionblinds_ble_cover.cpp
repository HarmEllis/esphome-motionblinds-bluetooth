#include "motionblinds_ble_cover.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.cover";

void MotionblindsBLECover::setup() {
  if (this->motor_ == nullptr)
    return;
  this->motor_->add_on_update_callback([this]() { this->update_state_(); });
  this->update_state_();
}

void MotionblindsBLECover::dump_config() { LOG_COVER("", "Motionblinds BLE Cover", this); }

cover::CoverTraits MotionblindsBLECover::get_traits() {
  cover::CoverTraits traits;
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  // The motor is only reachable in bursts, so between connections the position
  // is a remembered value rather than an observed one. The native API only
  // sends this flag when entities are listed, so it cannot track freshness at
  // runtime; the position_fresh binary sensor reports that instead.
  traits.set_is_assumed_state(true);
  return traits;
}

void MotionblindsBLECover::control(const cover::CoverCall &call) {
  if (this->motor_ == nullptr)
    return;

  if (call.get_stop()) {
    this->motor_->request_stop();
    return;
  }

  if (call.get_position().has_value()) {
    const auto &range = this->motor_->rail_range();
    const float position = *call.get_position();
    // COVER_OPEN is 1.0 and uncovers the window, which is the rail at the top
    // of its travel.
    const float window = range.window_max - position * (range.window_max - range.window_min);
    this->motor_->request_position(window);
  }
}

void MotionblindsBLECover::update_state_() {
  if (this->motor_ == nullptr)
    return;

  // See the top-down bottom-up cover: an unknown position is published as
  // nothing at all rather than as the default, which reads as fully open.
  const float window = this->motor_->window_position();
  if (std::isnan(window))
    return;

  const auto &range = this->motor_->rail_range();
  const float span = range.window_max - range.window_min;
  if (span > 0.0f)
    this->position = clamp((range.window_max - window) / span, 0.0f, 1.0f);

  // Closed is the rail at window_max, so travelling that way is closing.
  const int8_t direction = this->motor_->travel_direction();
  if (!this->motor_->is_moving() || direction == 0) {
    this->current_operation = cover::COVER_OPERATION_IDLE;
  } else {
    this->current_operation = direction > 0 ? cover::COVER_OPERATION_CLOSING : cover::COVER_OPERATION_OPENING;
  }
  this->publish_state(false);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
