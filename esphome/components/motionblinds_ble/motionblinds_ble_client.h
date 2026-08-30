#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/core/preferences.h"

namespace esphome::motionblinds_ble {

namespace espbt = esphome::esp32_ble_tracker;

class MotionblindsBLEMotor;

/* One BLE connection, owned by one motor.
 *
 * Deriving from BLEClientBase rather than reusing the ble_client component is
 * what lets a motor be described by a single YAML block, the way
 * bluetooth_proxy owns its own connections. Two further things fall out of it
 * that matter more than the tidier configuration:
 *
 *   - on_disconnect_complete() is a protected hook on BLEClientBase, so it is
 *     only reachable from a subclass. A ble_client node cannot see it and has
 *     to infer teardown from state changes instead.
 *   - the service objects are not released mid-connection, because that is
 *     driven by ble_client's "all nodes established" logic. Cached handles
 *     therefore stay resolvable for as long as the link lasts.
 *
 * The client is kept disabled while its motor has nothing to do. Enabling it
 * makes it eligible for the tracker's advertisement-driven promotion, which
 * connects exactly one client at a time and learns the motor's address type
 * from the advertisement — neither of which happens when connect() is called
 * directly.
 */
class MotionblindsBLEClient : public esp32_ble_client::BLEClientBase {
 public:
  /// Sentinel for "no code configured"; a real code is 16 bits.
  static constexpr uint32_t NO_MAC_CODE = 0xFFFFFFFF;

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_peer_preference_key(uint32_t key) { this->peer_preference_key_ = key; }

  /// Ask the controller to establish the link at a short, responsive
  /// connection interval. This is independent of fast_connect: it shortens
  /// the GATT work for both the conservative and optimistic handshakes.
  void set_low_latency_connection(bool enabled) { this->low_latency_connection_ = enabled; }

  /// Start the controller's initiator immediately when this peer's address
  /// and address type were learned earlier. The tracker
  /// still promotes only one DISCOVERED client at a time; this merely avoids
  /// spending one advertisement interval before entering that queue.
  void set_cached_connect(bool enabled) { this->cached_connect_ = enabled; }
  bool try_cached_connect();
  bool cached_connect_pending() const { return this->cached_attempt_; }

  /// Use ESP-IDF's enhanced open path with a full-duty initiator scan. This
  /// catches the next advertisement reliably and, unlike the legacy wrapper,
  /// gives us a supported way to cancel an unresolved open.
  void set_high_duty_cycle_connect(bool enabled) { this->high_duty_cycle_connect_ = enabled; }
  bool cancel_pending_connect();

  /// Identify the motor by the four-character code it advertises rather than
  /// by a full address.
  ///
  /// Motionblinds motors advertise as MOTION_XXXX, where XXXX is also the last
  /// two bytes of their address. That code is what the vendor app, the sticker
  /// on the motor and Home Assistant all show, so it is the identifier a user
  /// actually has to hand. The full address is learned from the first matching
  /// advertisement.
  void set_mac_code(uint16_t code) { this->mac_code_ = code; }

  /// Human-readable identifier used in every log line, so six motors can be
  /// told apart in one log.
  void set_label(const char *label) { this->label_ = label; }
  const char *label() const { return this->label_; }

  /// Forget a learned address so the next matching advertisement is adopted
  /// again. Only meaningful in mac_code mode.
  void forget_address();

  bool parse_device(const espbt::ESPBTDevice &device) override;
  void connect() override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  /// Enable or disable participation in scanning and connecting.
  void set_enabled(bool enabled);
  bool enabled() const { return this->enabled_; }

 protected:
  static constexpr uint8_t CACHED_PEER_VERSION = 1;
  struct CachedPeerIdentity {
    uint64_t address;
    uint8_t address_type;
    uint8_t version;
  } PACKED;

  void on_disconnect_complete(esp_err_t reason) override;
  void invalidate_cached_identity_();
  void clear_persisted_identity_();
  bool cached_peer_valid_(const CachedPeerIdentity &stored) const;
  void persist_cached_identity_();

  MotionblindsBLEMotor *motor_{nullptr};
  uint32_t mac_code_{NO_MAC_CODE};
  const char *label_{""};
  bool enabled_{false};
  bool low_latency_connection_{true};
  bool cached_connect_{true};
  bool cached_identity_valid_{false};
  bool cached_attempt_{false};
  bool high_duty_cycle_connect_{true};
  bool enhanced_open_pending_{false};
  bool cancel_open_pending_{false};
  uint32_t peer_preference_key_{0};
  ESPPreferenceObject peer_pref_;
  uint64_t persisted_address_{0};
  uint8_t persisted_address_type_{0xFF};
  bool restored_cached_identity_{false};
  bool persist_identity_pending_{false};
  bool clear_identity_pending_{false};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
