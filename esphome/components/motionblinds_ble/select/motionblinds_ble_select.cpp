#include "motionblinds_ble_select.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.select";

void MotionblindsBLESpeedSelect::setup() {
  if (this->motor_ == nullptr)
    return;
  this->motor_->add_on_update_callback([this]() { this->update_(); });
  this->update_();
}

void MotionblindsBLESpeedSelect::dump_config() { LOG_SELECT("", "Motionblinds BLE Speed", this); }

void MotionblindsBLESpeedSelect::control(const std::string &value) {
  if (this->motor_ == nullptr)
    return;

  SpeedLevel level;
  if (value == "low") {
    level = SpeedLevel::LOW;
  } else if (value == "high") {
    level = SpeedLevel::HIGH;
  } else {
    level = SpeedLevel::MEDIUM;
  }

  if (this->motor_->request_speed(level))
    this->publish_state(value);
}

void MotionblindsBLESpeedSelect::update_() {
  const auto speed = this->motor_->speed();
  if (!speed.has_value())
    return;

  const char *value = *speed == SpeedLevel::LOW ? "low" : (*speed == SpeedLevel::HIGH ? "high" : "medium");
  if (!(this->current_option() == value))
    this->publish_state(value);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
