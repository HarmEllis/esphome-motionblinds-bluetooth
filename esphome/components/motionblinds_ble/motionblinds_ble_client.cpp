#include "motionblinds_ble_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include "motionblinds_ble.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.client";

bool MotionblindsBLEClient::parse_device(const espbt::ESPBTDevice &device) {
  if (!this->enabled_)
    return false;

  // Configured by code rather than by address: adopt the address of the first
  // advertisement whose last two bytes match, then let the base class take it
  // from there. Doing it here rather than at connect time means the address
  // type is still learned from the advertisement, which is what makes a
  // randomised address work at all.
  if (this->mac_code_ != NO_MAC_CODE && this->address_ == 0) {
    if ((device.address_uint64() & 0xFFFF) != this->mac_code_)
      return false;
    ESP_LOGI(TAG, "Motion %04X is %s", static_cast<unsigned>(this->mac_code_),
             device.address_str().c_str());
    this->set_address(device.address_uint64());
  }

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
