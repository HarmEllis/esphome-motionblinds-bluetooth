#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/esp32_ble_client/ble_client_base.h"

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

  /// Ask the controller to establish the link at a short, responsive
  /// connection interval. This is independent of fast_connect: it shortens
  /// the GATT work for both the conservative and optimistic handshakes.
  void set_low_latency_connection(bool enabled) { this->low_latency_connection_ = enabled; }

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
  void on_disconnect_complete(esp_err_t reason) override;

  MotionblindsBLEMotor *motor_{nullptr};
  uint32_t mac_code_{NO_MAC_CODE};
  const char *label_{""};
  bool enabled_{false};
  bool low_latency_connection_{true};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
