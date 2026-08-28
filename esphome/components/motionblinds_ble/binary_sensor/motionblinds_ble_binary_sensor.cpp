#include "motionblinds_ble_binary_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.binary_sensor";

void MotionblindsBLEBinarySensor::setup() {
  if (this->motor_ == nullptr)
    return;
  this->motor_->add_on_update_callback([this]() { this->update_(); });
  this->update_();
}

void MotionblindsBLEBinarySensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE binary sensors");
  LOG_BINARY_SENSOR("  ", "Position fresh", this->fresh_);
  LOG_BINARY_SENSOR("  ", "Calibrated", this->calibrated_);
}

void MotionblindsBLEBinarySensor::update_() {
  if (this->fresh_ != nullptr)
    this->fresh_->publish_state(this->motor_->position_fresh());
  // Reported as a problem: an uncalibrated motor refuses position commands.
  // Left unpublished until the motor has actually said something about its end
  // positions. Publishing the enum's zero value would announce "uncalibrated"
  // on a motor nobody has spoken to yet, and nothing persists this, so on a
  // blind that is not touched for a day that claim would simply stand.
  if (this->calibrated_ != nullptr && this->motor_->calibration_known())
    this->calibrated_->publish_state(!this->motor_->calibrated());
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
