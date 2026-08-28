#include "motionblinds_ble_tdbu_number.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble_tdbu {

static const char *const TAG = "motionblinds_ble_tdbu.number";

void MotionblindsBLETdbuNumber::setup() {
  if (this->tdbu_ == nullptr)
    return;
  this->tdbu_->add_on_update_callback([this]() { this->update_state_(); });
  this->update_state_();
}

void MotionblindsBLETdbuNumber::dump_config() { LOG_NUMBER("", "Motionblinds BLE TDBU Fabric Position", this); }

void MotionblindsBLETdbuNumber::control(float value) {
  if (this->tdbu_ == nullptr)
    return;
  this->tdbu_->set_fabric_centre(value);
  this->publish_state(value);
}

void MotionblindsBLETdbuNumber::update_state_() {
  if (this->tdbu_ == nullptr)
    return;
  const float centre = this->tdbu_->fabric_centre();
  if (!std::isnan(centre) && (std::isnan(this->state) || std::fabs(this->state - centre) > 0.5f))
    this->publish_state(centre);
}

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
