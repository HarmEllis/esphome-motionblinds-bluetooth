#include "motionblinds_ble_text_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.text_sensor";

void MotionblindsBLETextSensor::setup() {
  if (this->motor_ == nullptr)
    return;
  this->motor_->add_on_update_callback([this]() { this->update_(); });
  this->update_();
}

void MotionblindsBLETextSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE text sensors");
  LOG_TEXT_SENSOR("  ", "Connection status", this->status_);
}

void MotionblindsBLETextSensor::update_() {
  if (this->status_ == nullptr)
    return;
  const char *state = motor_state_to_string(this->motor_->state());
  if (this->status_->state != state)
    this->status_->publish_state(state);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
