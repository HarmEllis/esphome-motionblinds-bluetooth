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
// ESP-IDF's legacy initiator listens for 30ms every 60ms. These parameters
// listen continuously in 40ms periods while an open is pending. This consumes
// more radio time on the ESP, not on the battery motor, and only until the
// target's next advertisement arrives.
static constexpr uint16_t INITIATOR_SCAN_INTERVAL = 0x40;  // 40ms
static constexpr uint16_t INITIATOR_SCAN_WINDOW = 0x40;    // 40ms (100%)
static constexpr uint16_t DEFAULT_MIN_INTERVAL = 0x0A;     // 12.5ms
static constexpr uint16_t DEFAULT_MAX_INTERVAL = 0x0C;     // 15ms
static constexpr uint16_t DEFAULT_TIMEOUT = 600;           // 6s

void MotionblindsBLEClient::setup() {
  BLEClientBase::setup();
  this->peer_pref_ = global_preferences->make_preference<CachedPeerIdentity>(this->peer_preference_key_);

  // Restoring a random address is only safe because enhanced open can be
  // cancelled. With the legacy path, keep the useful same-boot cache but make
  // the first connection after every reboot learn the address type normally.
  if (!this->cached_connect_ || !this->high_duty_cycle_connect_)
    return;

  CachedPeerIdentity stored{};
  if (!this->peer_pref_.load(&stored) || !this->cached_peer_valid_(stored))
    return;

  this->set_address(stored.address);
  this->remote_addr_type_ = static_cast<esp_ble_addr_type_t>(stored.address_type);
  this->cached_identity_valid_ = true;
  this->restored_cached_identity_ = true;
  this->persisted_address_ = stored.address;
  this->persisted_address_type_ = stored.address_type;
  ESP_LOGI(TAG, "[%s] Restored the learned BLE identity", this->label_);
}

void MotionblindsBLEClient::loop() {
  BLEClientBase::loop();
  if (this->clear_identity_pending_) {
    this->clear_identity_pending_ = false;
    this->persist_identity_pending_ = false;
    this->clear_persisted_identity_();
  } else if (this->persist_identity_pending_) {
    this->persist_identity_pending_ = false;
    this->persist_cached_identity_();
  }
}

void MotionblindsBLEClient::dump_config() {
  BLEClientBase::dump_config();
  ESP_LOGCONFIG(TAG,
                "  Cached connect: %s\n"
                "  High-duty initiator: %s",
                YESNO(this->cached_connect_), YESNO(this->high_duty_cycle_connect_));
  if (this->restored_cached_identity_)
    ESP_LOGI(TAG, "[%s] Cached BLE identity survived the restart", this->label_);
}

void MotionblindsBLEClient::connect() {
  this->cancel_open_pending_ = false;
  if (!this->high_duty_cycle_connect_) {
    if (this->low_latency_connection_)
      this->set_conn_params_(LOW_LATENCY_MIN_INTERVAL, LOW_LATENCY_MAX_INTERVAL, 0, LOW_LATENCY_TIMEOUT,
                             "motionblinds low-latency");
    BLEClientBase::connect();
  } else {
    const uint16_t min_interval = this->low_latency_connection_ ? LOW_LATENCY_MIN_INTERVAL : DEFAULT_MIN_INTERVAL;
    const uint16_t max_interval = this->low_latency_connection_ ? LOW_LATENCY_MAX_INTERVAL : DEFAULT_MAX_INTERVAL;
    const uint16_t timeout = this->low_latency_connection_ ? LOW_LATENCY_TIMEOUT : DEFAULT_TIMEOUT;

    esp_ble_conn_params_t conn_params{};
    conn_params.scan_interval = INITIATOR_SCAN_INTERVAL;
    conn_params.scan_window = INITIATOR_SCAN_WINDOW;
    conn_params.interval_min = min_interval;
    conn_params.interval_max = max_interval;
    conn_params.latency = 0;
    conn_params.supervision_timeout = timeout;

    esp_ble_gatt_creat_conn_params_t open_params{};
    memcpy(open_params.remote_bda, this->remote_bda_, sizeof(esp_bd_addr_t));
    open_params.remote_addr_type = this->remote_addr_type_;
    open_params.is_direct = true;
    open_params.is_aux = false;
    open_params.own_addr_type = static_cast<esp_ble_addr_type_t>(0xFF);  // let the stack choose
    open_params.phy_mask = ESP_BLE_PHY_1M_PREF_MASK;
    open_params.phy_1m_conn_params = &conn_params;

    ESP_LOGI(TAG, "[%s] Connecting with a 100%% initiator scan window", this->label_);
    this->paired_ = false;
    this->enable_loop();
    this->set_state(espbt::ClientState::CONNECTING);
    this->enhanced_open_pending_ = true;
    const esp_err_t result = esp_ble_gattc_enh_open(this->gattc_if_, &open_params);
    if (result != ESP_OK)
      this->enhanced_open_pending_ = false;
    this->handle_connection_result_(result);
  }

  // A controller-level rejection can return synchronously and produces no
  // OPEN event, so handle that form of failed shortcut here as well.
  if (this->cached_attempt_ && this->state() == espbt::ClientState::IDLE)
    this->invalidate_cached_identity_();
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
  if (ours) {
    // BLEClientBase has now copied both the address and its type. Keep that
    // pair in RAM after disconnect: the next request can put this client in
    // the tracker's DISCOVERED queue immediately, so the initiator catches the
    // motor's next advertisement instead of first waiting for one merely to
    // learn information it already has.
    this->cached_identity_valid_ = true;
    this->cached_attempt_ = false;
    if (this->motor_ != nullptr) {
      // The advertisement is the only place a signal strength exists; once
      // connected there is nothing further to measure it from.
      this->motor_->set_signal_strength(device.get_rssi());
    }
  }
  return ours;
}

bool MotionblindsBLEClient::try_cached_connect() {
  if (!this->cached_connect_ || !this->cached_identity_valid_ || !this->enabled_ || this->address_ == 0 ||
      this->state() != espbt::ClientState::IDLE)
    return false;

  // Do not call connect() directly. DISCOVERED is the tracker's serialised
  // queue: it stops scanning and promotes exactly one client, preserving the
  // behaviour that keeps several motors from opening simultaneously.
  this->cached_attempt_ = true;
  ESP_LOGI(TAG, "[%s] Reusing the learned BLE identity; waiting directly for its next advertisement", this->label_);
  this->set_state(espbt::ClientState::DISCOVERED);
  return true;
}

bool MotionblindsBLEClient::cancel_pending_connect() {
  if (!this->high_duty_cycle_connect_ || !this->enhanced_open_pending_ || this->cancel_open_pending_ ||
      this->state() != espbt::ClientState::CONNECTING)
    return this->cancel_open_pending_;

  esp_ble_gattc_cancel_open_params_t params{};
  params.gattc_if = this->gattc_if_;
  memcpy(params.remote_bda, this->remote_bda_, sizeof(esp_bd_addr_t));
  const esp_err_t result = esp_ble_gattc_cancel_open(&params);
  if (result != ESP_OK) {
    // Do not hammer the controller from every loop iteration. A later OPEN
    // event clears this guard; otherwise the long stuck timeout remains.
    this->cancel_open_pending_ = true;
    ESP_LOGW(TAG, "[%s] Could not cancel the pending BLE connection: %d", this->label_, result);
    return false;
  }
  this->cancel_open_pending_ = true;
  ESP_LOGW(TAG, "[%s] Cancelling the BLE connection that did not resolve in time", this->label_);
  return true;
}

bool MotionblindsBLEClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;

  if (event == ESP_GATTC_OPEN_EVT) {
    this->enhanced_open_pending_ = false;
    this->cancel_open_pending_ = false;
  }

  if (event == ESP_GATTC_OPEN_EVT && this->cached_attempt_) {
    this->cached_attempt_ = false;
    if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
      // An address (especially a random one) can stop being valid. A failed
      // shortcut gets one chance only; the motor's existing retry then returns
      // to advertisement-driven discovery and relearns both address and type.
      this->invalidate_cached_identity_();
    }
  }

  if (event == ESP_GATTC_CANCEL_OPEN_EVT &&
      memcmp(param->cancel_open.remote_bda, this->remote_bda_, sizeof(esp_bd_addr_t)) == 0) {
    if (param->cancel_open.status == ESP_GATT_OK) {
      this->cancel_open_pending_ = false;
      this->enhanced_open_pending_ = false;
      if (this->cached_attempt_)
        this->invalidate_cached_identity_();
      if (this->state() == espbt::ClientState::CONNECTING)
        this->set_idle_();
    } else {
      // Keep the guard set: retrying this API on every component loop only
      // floods the same controller queue. OPEN may still arrive and clear it.
      this->cancel_open_pending_ = true;
      ESP_LOGW(TAG, "[%s] BLE connection cancellation returned status %d", this->label_,
               param->cancel_open.status);
    }
  }
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
    this->cached_attempt_ = false;
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
  this->cached_identity_valid_ = false;
  this->cached_attempt_ = false;
  this->set_address(0);
}

void MotionblindsBLEClient::invalidate_cached_identity_() {
  ESP_LOGW(TAG, "[%s] Cached BLE identity did not connect; falling back to advertisement discovery", this->label_);
  this->cached_identity_valid_ = false;
  this->cached_attempt_ = false;
  if (this->mac_code_ != NO_MAC_CODE)
    this->set_address(0);

  this->persisted_address_ = 0;
  this->persisted_address_type_ = 0xFF;
  this->restored_cached_identity_ = false;
  // Defer the tombstone too: failed OPEN and CANCEL events are GATT callbacks,
  // where synchronous flash I/O would delay the Bluetooth task.
  this->clear_identity_pending_ = true;
  this->enable_loop();
}

void MotionblindsBLEClient::clear_persisted_identity_() {
  if (this->peer_preference_key_ == 0)
    return;
  // Version zero is an explicit tombstone and cannot validate on next boot.
  CachedPeerIdentity empty{};
  if (!this->peer_pref_.save(&empty) || !global_preferences->sync())
    ESP_LOGW(TAG, "[%s] Could not erase the stale BLE identity", this->label_);
}

bool MotionblindsBLEClient::cached_peer_valid_(const CachedPeerIdentity &stored) const {
  if (stored.version != CACHED_PEER_VERSION || stored.address == 0 || (stored.address >> 48) != 0 ||
      stored.address_type > BLE_ADDR_TYPE_RPA_RANDOM)
    return false;
  if (this->mac_code_ != NO_MAC_CODE)
    return (stored.address & 0xFFFF) == this->mac_code_;
  return this->address_ != 0 && stored.address == this->address_;
}

void MotionblindsBLEClient::persist_cached_identity_() {
  if (!this->cached_connect_ || !this->high_duty_cycle_connect_ || !this->cached_identity_valid_ ||
      this->peer_preference_key_ == 0)
    return;
  const uint8_t address_type = static_cast<uint8_t>(this->remote_addr_type_);
  if (this->persisted_address_ == this->address_ && this->persisted_address_type_ == address_type)
    return;

  const CachedPeerIdentity stored{this->address_, address_type, CACHED_PEER_VERSION};
  if (!this->peer_pref_.save(&stored) || !global_preferences->sync()) {
    ESP_LOGW(TAG, "[%s] Could not persist the learned BLE identity", this->label_);
    return;
  }
  this->persisted_address_ = this->address_;
  this->persisted_address_type_ = address_type;
  ESP_LOGI(TAG, "[%s] Stored the learned BLE identity for the next restart", this->label_);
}

void MotionblindsBLEClient::on_disconnect_complete(esp_err_t reason) {
  // Flash I/O does not belong in a GATT callback. Schedule the commit for the
  // normal component loop, after this radio link has been torn down.
  this->persist_identity_pending_ = true;
  this->enable_loop();
  if (this->motor_ != nullptr)
    this->motor_->on_disconnect_complete(reason);
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
