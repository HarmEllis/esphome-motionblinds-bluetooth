#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <vector>

#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "motionblinds_ble_client.h"
#include "motionblinds_protocol.h"
#include "motionblinds_rail.h"

namespace esphome::motionblinds_ble {

enum class BlindType : uint8_t {
  ROLLER,
  HONEYCOMB,
  ROMAN,
  VENETIAN,
  DOUBLE_ROLLER,
  CURTAIN,
  VERTICAL,
};

/// Lifecycle of one motor. Every state except IDLE and READY has a deadline,
/// and an operation as a whole has one too; the Home Assistant integration
/// this component replaces has neither, which is why it can hang forever.
enum class MotorState : uint8_t {
  IDLE,
  DISCOVERING,
  CONNECTING,
  HANDSHAKE,
  READY,
  DISCONNECTING,
  FAILED,
};

/// How far a command is followed before it counts as done.
///
/// The command characteristic is written without a response, so a successful
/// write says nothing at all about whether the motor acted. Keeping the levels
/// distinct is what stops this component from repeating the failure mode where
/// a command is reported as successful while the blind never moves.
enum class Verification : uint8_t {
  ACKED,    ///< the local stack finished the write. Not a peer acknowledgement.
  STARTED,  ///< the motor sent us something back
  SETTLED,  ///< the motor reported it reached the target and stayed there
};

const char *motor_state_to_string(MotorState state);

class MotionblindsBLEMotor : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  /// Forwarded by the client that owns this motor's connection.
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  /// Called once the link is fully torn down, from the close event and from
  /// the client's own lost-event watchdog alike.
  void on_disconnect_complete(esp_err_t reason);

  // ---------------------------------------------------------------- config
  void set_ble_client(MotionblindsBLEClient *client) { this->ble_client_ = client; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
  /// Key under which this motor's position is stored. Derived from the MAC so
  /// it stays stable across reflashes and cannot collide between motors.
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }
  void set_blind_type(BlindType type) { this->blind_type_ = type; }
  void set_rail_range(float window_min, float window_max, bool invert) {
    this->range_.window_min = window_min;
    this->range_.window_max = window_max;
    this->range_.invert = invert;
  }
  void set_disconnect_delay(uint32_t ms) { this->disconnect_delay_ = ms; }
  void set_discovery_timeout(uint32_t ms) { this->discovery_timeout_ = ms; }
  void set_connect_timeout(uint32_t ms) { this->connect_timeout_ = ms; }
  void set_handshake_timeout(uint32_t ms) { this->handshake_timeout_ = ms; }
  void set_operation_timeout(uint32_t ms) { this->operation_timeout_ = ms; }
  void set_stuck_connect_timeout(uint32_t ms) { this->stuck_connect_timeout_ = ms; }
  /// How many bounded discovery attempts before giving up. One window is too
  /// fragile for a motor that advertises weakly or rarely.
  void set_discovery_rounds(uint8_t rounds) { this->discovery_rounds_ = rounds; }
  void set_label(const char *label) { this->label_ = label; }
  const char *label() const { return this->label_; }
  void set_recover_by_reboot(bool enabled, uint32_t after_ms) {
    this->recover_by_reboot_ = enabled;
    this->recover_after_ = after_ms;
  }

  // ------------------------------------------------------------- commands
  /// Drive the rail to a window position. Returns false when the request was
  /// rejected outright, which is not the same as the motor failing later.
  bool request_position(float window_position);
  bool request_open();
  bool request_close();
  bool request_stop();
  bool request_favorite();
  bool request_speed(SpeedLevel level);
  bool request_status();
  void request_connect();
  void request_disconnect();

  // ---------------------------------------------------------------- lease
  /// Hold the connection open across a multi-step operation.
  ///
  /// Without this the idle timer can disconnect a motor in the middle of the
  /// very move it is supposed to be watching: the queue drains the moment a
  /// command is dispatched, while the rail keeps travelling for seconds
  /// afterwards. A disconnected motor reports nothing, so both the freshness
  /// of its position and the clearance watchdog would quietly stop working.
  void acquire_lease();
  void release_lease();
  bool leased() const { return this->lease_count_ > 0; }

  // ----------------------------------------------------------------- state
  MotorState state() const { return this->state_; }
  const RailRange &rail_range() const { return this->range_; }
  BlindType blind_type() const { return this->blind_type_; }

  /// Last known position in window coordinates, or NAN if never seen.
  float window_position() const;
  /// Whether that position was observed during the current connection. A
  /// position restored from flash is good enough to show, never to move on.
  bool position_fresh() const { return this->position_fresh_; }
  bool is_moving() const { return this->moving_; }
  /// Whether this motor still owes the caller anything: a queued command, or
  /// one whose outcome is not yet established.
  bool busy() const { return !this->queue_.empty() || this->command_in_flight_; }
  /// -1 while travelling towards the top of the window, +1 towards the bottom,
  /// 0 when not moving.
  int8_t travel_direction() const { return this->travel_direction_; }
  bool calibrated() const { return this->end_positions_ == EndPositions::BOTH; }
  bool favorite_set() const { return this->favorite_set_; }

  optional<uint8_t> battery_percentage() const;
  optional<bool> battery_charging() const;
  optional<SpeedLevel> speed() const;
  optional<int8_t> signal_strength() const;
  void set_signal_strength(int8_t rssi);

  void add_on_update_callback(std::function<void()> &&callback) { this->update_callback_.add(std::move(callback)); }

 protected:
  struct PendingCommand {
    Command command;
    uint8_t argument;
    Verification verification;
    uint8_t target;  ///< raw target position, for SETTLED verification
  };

  /// Handshake progress. Notifications are not usable until the descriptor
  /// write that enables them has actually been confirmed.
  enum class Handshake : uint8_t {
    NONE,
    WAIT_NOTIFY_REGISTRATION,
    WAIT_DESCRIPTOR_WRITE,
    WAIT_BLIND_SETTLE,
    WAIT_STATUS,
    DONE,
  };

  struct PersistedPosition {
    uint8_t raw_position;
    uint8_t raw_tilt;
    bool valid;
  } PACKED;

  bool enqueue_(Command command, uint8_t argument, Verification verification, uint8_t target = 0);
  void start_operation_();
  void finish_operation_();
  void fail_(const char *reason);
  void abort_();
  void reconcile_state_();
  void drive_handshake_();
  void dispatch_();
  bool write_command_(Command command, uint8_t argument);
  void handle_notification_(const uint8_t *data, uint16_t length);
  void apply_notification_(const Notification &notification);
  void set_state_(MotorState state);
  void mark_stale_();
  void save_position_();
  void publish_();

  MotionblindsBLEClient *ble_client_{nullptr};
  uint32_t preference_key_{0};
  time::RealTimeClock *time_{nullptr};
  BlindType blind_type_{BlindType::ROLLER};
  RailRange range_{};

  uint32_t disconnect_delay_{15000};
  uint32_t discovery_timeout_{30000};
  uint32_t connect_timeout_{20000};
  uint32_t handshake_timeout_{15000};
  uint32_t operation_timeout_{120000};
  uint32_t stuck_connect_timeout_{60000};
  uint32_t recover_after_{300000};
  uint8_t discovery_rounds_{3};
  bool recover_by_reboot_{false};
  const char *label_{""};

  MotorState state_{MotorState::IDLE};
  Handshake handshake_{Handshake::NONE};
  uint32_t state_since_{0};
  uint32_t operation_since_{0};
  uint32_t last_activity_{0};
  uint32_t command_sent_at_{0};
  /// Fixed when the command goes out. Recomputing it from the live position
  /// would shrink the deadline as the rail approaches its target.
  uint32_t command_budget_{0};
  uint32_t settle_since_{0};
  uint32_t connecting_since_{0};
  uint8_t attempts_{0};
  uint8_t discovery_round_{0};
  uint32_t discovery_scanning_ms_{0};
  uint32_t discovery_last_tick_{0};
  uint32_t backoff_until_{0};

  uint16_t command_handle_{0};
  uint16_t notify_handle_{0};
  uint16_t config_descriptor_handle_{0};

  std::vector<PendingCommand> queue_;
  bool command_in_flight_{false};
  bool stuck_reported_{false};
  bool settle_rechecked_{false};
  int8_t travel_direction_{0};
  PendingCommand in_flight_{};
  uint8_t settle_matches_{0};

  uint16_t lease_count_{0};

  bool has_position_{false};
  bool position_fresh_{false};
  bool moving_{false};
  uint8_t raw_position_{0};
  uint8_t raw_tilt_{0};
  EndPositions end_positions_{EndPositions::NONE};
  bool favorite_set_{false};

  bool has_signal_{false};
  int8_t signal_strength_{0};
  bool has_battery_{false};
  uint8_t battery_percentage_{0};
  bool battery_charging_{false};
  bool has_speed_{false};
  SpeedLevel speed_{SpeedLevel::MEDIUM};

  ESPPreferenceObject position_pref_;
  CallbackManager<void()> update_callback_{};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
