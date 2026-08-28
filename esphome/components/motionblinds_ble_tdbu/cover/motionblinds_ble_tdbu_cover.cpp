#include "motionblinds_ble_tdbu_cover.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble_tdbu {

static const char *const TAG = "motionblinds_ble_tdbu.cover";

void MotionblindsBLETdbuCover::setup() {
  if (this->tdbu_ == nullptr)
    return;
  this->tdbu_->add_on_update_callback([this]() { this->update_state_(); });
  this->update_state_();
}

void MotionblindsBLETdbuCover::dump_config() { LOG_COVER("", "Motionblinds BLE TDBU Cover", this); }

cover::CoverTraits MotionblindsBLETdbuCover::get_traits() {
  cover::CoverTraits traits;
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  // These motors are only reachable in bursts, so between connections the
  // position is remembered rather than observed.
  traits.set_is_assumed_state(true);
  return traits;
}

void MotionblindsBLETdbuCover::control(const cover::CoverCall &call) {
  if (this->tdbu_ == nullptr)
    return;

  if (call.get_stop()) {
    if (this->rail_ == CoverRail::COMBINED) {
      this->tdbu_->stop_all();
    } else {
      this->tdbu_->stop_rail(this->rail_ == CoverRail::TOP ? Rail::TOP : Rail::BOTTOM);
    }
    return;
  }

  if (call.get_position().has_value()) {
    const float position = *call.get_position();
    if (this->rail_ == CoverRail::COMBINED) {
      this->tdbu_->set_combined_openness(position);
    } else {
      this->tdbu_->set_rail_position(this->rail_ == CoverRail::TOP ? Rail::TOP : Rail::BOTTOM, position);
    }
  }
}

void MotionblindsBLETdbuCover::update_state_() {
  if (this->tdbu_ == nullptr)
    return;

  float value;
  if (this->rail_ == CoverRail::COMBINED) {
    value = this->tdbu_->combined_openness();
  } else {
    value = this->tdbu_->rail_position(this->rail_ == CoverRail::TOP ? Rail::TOP : Rail::BOTTOM);
  }

  // Nothing is published while the position is unknown. A cover position is a
  // plain number with no room for "I do not know", so publishing anything at
  // all would state a position the component does not have — and the default
  // reads as fully open, which is the most misleading answer available. Leaving
  // the entity unknown is the honest one, and it is what the blind's status
  // text says too.
  if (std::isnan(value))
    return;

  // Whatever the rail actually reached, including a target the collision guard
  // had to shorten. Publishing the request instead would tell Home Assistant a
  // rail is at an end stop it never got to.
  this->position = clamp(value, 0.0f, 1.0f);

  const int8_t direction = this->tdbu_->travel_direction();
  if (!this->tdbu_->is_moving() || direction == 0) {
    this->current_operation = cover::COVER_OPERATION_IDLE;
  } else {
    this->current_operation = direction > 0 ? cover::COVER_OPERATION_CLOSING : cover::COVER_OPERATION_OPENING;
  }
  this->publish_state(false);
}

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
