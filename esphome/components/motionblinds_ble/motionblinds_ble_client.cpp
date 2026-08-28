#include "motionblinds_ble_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include "motionblinds_ble.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.client";

bool MotionblindsBLEClient::parse_device(const espbt::ESPBTDevice &device) {
  if (!this->enabled_)
    return false;
  const bool ours = BLEClientBase::parse_device(device);
  if (ours && this->motor_ != nullptr) {
    // The advertisement is the only place a signal strength exists; once
    // connected there is nothing further to measure it from.
    this->motor_->set_signal_strength(device.get_rssi());
  }
  return ours;
}

bool MotionblindsBLEClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;
  if (this->motor_ != nullptr)
    this->motor_->gattc_event_handler(event, gattc_if, param);
  return true;
}

void MotionblindsBLEClient::set_enabled(bool enabled) {
  if (this->enabled_ == enabled)
    return;
  this->enabled_ = enabled;

  if (enabled) {
    ESP_LOGD(TAG, "[%s] Listening for advertisements", this->address_str());
    return;
  }

  ESP_LOGD(TAG, "[%s] No longer wanted", this->address_str());
  if (this->state() == espbt::ClientState::DISCOVERED) {
    // Promotion does not consult the enabled flag, so a client left in
    // DISCOVERED would still be connected only to be dropped again. Stepping
    // back to IDLE is safe here precisely because nothing has been handed to
    // the Bluetooth stack yet.
    this->set_state(espbt::ClientState::IDLE);
    return;
  }
  this->disconnect();
}

void MotionblindsBLEClient::on_disconnect_complete(esp_err_t reason) {
  if (this->motor_ != nullptr)
    this->motor_->on_disconnect_complete(reason);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
