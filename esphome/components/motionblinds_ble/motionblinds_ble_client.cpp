#include "motionblinds_ble_client.h"

#ifdef USE_ESP32

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

#include "motionblinds_ble.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.client";

// The same balanced interval ESPHome uses for its cached V3 clients. The
// legacy client path this component needs otherwise leaves the ESP-IDF default
// (12.5-15ms) in place. Setting the preference before opening the link removes
// idle air time from service discovery and the notification/key handshake
// without adding another connection-parameter round trip.
static constexpr uint16_t LOW_LATENCY_MIN_INTERVAL = 0x07;  // 8.75ms
static constexpr uint16_t LOW_LATENCY_MAX_INTERVAL = 0x09;  // 11.25ms
static constexpr uint16_t LOW_LATENCY_TIMEOUT = 800;        // 8s

void MotionblindsBLEClient::connect() {
  if (this->low_latency_connection_)
    this->set_conn_params_(LOW_LATENCY_MIN_INTERVAL, LOW_LATENCY_MAX_INTERVAL, 0, LOW_LATENCY_TIMEOUT,
                           "motionblinds low-latency");
  BLEClientBase::connect();
}

bool MotionblindsBLEClient::parse_device(const espbt::ESPBTDevice &device) {
  if (!this->enabled_)
    return false;

  // Configured by code rather than by address: adopt the address of the first
  // advertisement whose last two bytes match, then let the base class take it
  // from there. Doing it here rather than at connect time means the address
  // type is still learned from the advertisement, which is what makes a
  // randomised address work at all.
  if (this->mac_code_ != NO_MAC_CODE && this->address_ == 0) {
    // The low two bytes of the address are the code, but they are only two
    // bytes: another device could carry the same pair by coincidence, and once
    // adopted the wrong address sticks — that device keeps advertising, so the
    // timeout that would forget it never comes. When the advertisement carries
    // a name, it has to be this motor's.
    const auto name = device.get_name();
    if (name.size() >= 7 && memcmp(name.c_str(), "MOTION_", 7) != 0)
      return false;  // some other device entirely

    if ((device.address_uint64() & 0xFFFF) != this->mac_code_) {
      // Report the near misses too. "Not seen on air" is only actionable if you
      // can tell a motor that never advertised from one that was heard clearly
      // and rejected.
      const auto name = device.get_name();
      if (name.size() >= 7 && memcmp(name.c_str(), "MOTION_", 7) == 0) {
        char address[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
        ESP_LOGD(TAG, "[%s] Heard %.*s (%s) at %d dBm, not my code %04X", this->label_,
                 static_cast<int>(name.size()), name.c_str(), device.address_str_to(address), device.get_rssi(),
                 static_cast<unsigned>(this->mac_code_));
      }
      return false;
    }
    // A name, when present, must spell out this exact code. Motors have been
    // seen advertising without one, so its absence is not disqualifying.
    char expected[8];
    snprintf(expected, sizeof(expected), "%04X", static_cast<unsigned>(this->mac_code_));
    if (name.size() >= 11 && memcmp(name.c_str() + 7, expected, 4) != 0) {
      ESP_LOGW(TAG, "[%s] %.*s carries code %04X in its address but not in its name; ignoring it", this->label_,
               static_cast<int>(name.size()), name.c_str(), static_cast<unsigned>(this->mac_code_));
      return false;
    }

    this->set_address(device.address_uint64());
    ESP_LOGI(TAG, "[%s] Motion %04X is %s at %d dBm%s", this->label_, static_cast<unsigned>(this->mac_code_),
             this->address_str(), device.get_rssi(), name.size() >= 11 ? "" : " (unnamed advertisement)");
  } else if (this->address_ != 0 && device.address_uint64() == this->address_) {
    ESP_LOGD(TAG, "[%s] Heard at %d dBm", this->label_, device.get_rssi());
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
    ESP_LOGD(TAG, "[%s] Listening for advertisements", this->label_);
    return;
  }

  ESP_LOGD(TAG, "[%s] No longer wanted", this->label_);
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

void MotionblindsBLEClient::forget_address() {
  if (this->mac_code_ == NO_MAC_CODE || this->address_ == 0)
    return;
  ESP_LOGW(TAG, "[%s] Forgetting address %s; will adopt the next advertisement matching %04X", this->label_,
           this->address_str(), static_cast<unsigned>(this->mac_code_));
  this->set_address(0);
}

void MotionblindsBLEClient::on_disconnect_complete(esp_err_t reason) {
  if (this->motor_ != nullptr)
    this->motor_->on_disconnect_complete(reason);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
