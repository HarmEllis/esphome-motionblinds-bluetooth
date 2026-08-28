#include "motionblinds_ble_sensor.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.sensor";

void MotionblindsBLESensor::setup() {
  if (this->motor_ == nullptr)
    return;
  this->motor_->add_on_update_callback([this]() { this->update_(); });
}

void MotionblindsBLESensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE sensors");
  LOG_SENSOR("  ", "Battery", this->battery_);
  LOG_SENSOR("  ", "Signal strength", this->signal_);
}

void MotionblindsBLESensor::update_() {
  if (this->battery_ != nullptr) {
    if (const auto battery = this->motor_->battery_percentage())
      this->battery_->publish_state(*battery);
  }
  if (this->signal_ != nullptr) {
    if (const auto rssi = this->motor_->signal_strength())
      this->signal_->publish_state(*rssi);
  }
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
