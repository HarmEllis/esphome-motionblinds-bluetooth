#include "motionblinds_tdbu_diagnostics.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble_tdbu {

static const char *const TAG = "motionblinds_ble_tdbu.diagnostics";

void MotionblindsBLETdbuDiagnostics::setup() {
  if (this->tdbu_ == nullptr)
    return;
  this->tdbu_->add_on_update_callback([this]() {
    this->publish_(Rail::TOP);
    this->publish_(Rail::BOTTOM);
  });
  this->publish_(Rail::TOP);
  this->publish_(Rail::BOTTOM);
}

void MotionblindsBLETdbuDiagnostics::dump_config() { ESP_LOGCONFIG(TAG, "Motionblinds BLE TDBU diagnostics"); }

void MotionblindsBLETdbuDiagnostics::publish_(Rail rail) {
  auto *motor = this->tdbu_ == nullptr ? nullptr : this->tdbu_->motor(rail);
  if (motor == nullptr)
    return;

  RailEntities &entities = this->rail_(rail);

#ifdef USE_SENSOR
  if (entities.battery != nullptr) {
    if (const auto battery = motor->battery_percentage())
      entities.battery->publish_state(*battery);
  }
  if (entities.signal != nullptr) {
    if (const auto rssi = motor->signal_strength())
      entities.signal->publish_state(*rssi);
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (entities.fresh != nullptr)
    entities.fresh->publish_state(motor->position_fresh());
#endif
#ifdef USE_TEXT_SENSOR
  if (entities.connection != nullptr) {
    const char *state = esphome::motionblinds_ble::motor_state_to_string(motor->state());
    if (entities.connection->state != state)
      entities.connection->publish_state(state);
  }
#endif
}

#ifdef USE_BUTTON
void MotionblindsBLETdbuRefreshButton::dump_config() { LOG_BUTTON("", "Motionblinds BLE TDBU Refresh", this); }

void MotionblindsBLETdbuRefreshButton::press_action() {
  if (this->tdbu_ == nullptr)
    return;
  if (auto *motor = this->tdbu_->motor(this->rail_))
    motor->request_status();
}
#endif

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
