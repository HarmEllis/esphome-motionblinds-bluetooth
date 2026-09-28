#pragma once
// Motor test double: drive outcomes explicitly while running the production
// coordinator, including its planner, leases, callbacks and loop.
#include <optional>
#include "esphome/components/motionblinds_ble/motionblinds_rail.h"
#include "esphome/core/helpers.h"
namespace esphome::motionblinds_ble {
enum class MotorState { IDLE, DISCOVERING, CONNECTING, READY, FAILED };
class MotionblindsBLEMotor {
 public:
  float position{0};
  bool fresh{true};
  bool pending{false};
  bool written{false};
  MotorState current_state{MotorState::IDLE};
  int leases{0};
  int stops{0};
  int commands{0};
  int motion_status_queries{0};
  float target{0};
  std::optional<int> speed_level{};
  RailRange range{};
  CallbackManager<void()> updates;
  const RailRange &rail_range() const { return range; }
  float window_position() const { return position; }
  bool position_fresh() const { return fresh; }
  bool busy() const { return pending; }
  bool is_moving() const { return pending && written; }
  int8_t travel_direction() const { return 0; }
  MotorState state() const { return current_state; }
  std::optional<int> speed() const { return speed_level; }
  bool position_write_sent() const { return pending && written; }
  bool command_origin(uint8_t &, bool &) const { return false; }
  void acquire_lease() { leases++; current_state = MotorState::READY; }
  void release_lease() { if (leases > 0) leases--; }
  bool request_position(float value, uint32_t) {
    target = value;
    pending = true;
    written = false;
    commands++;
    return true;
  }
  bool request_status() { pending = true; return true; }
  bool request_motion_status() { motion_status_queries++; return pending && written; }
  bool request_stop() { stops++; pending = false; written = false; updates.call(); return true; }
  void add_on_update_callback(std::function<void()> callback) { updates.add(std::move(callback)); }
  void complete(float value) { position = value; pending = false; fresh = true; updates.call(); }
  void fail() { current_state = MotorState::FAILED; pending = false; fresh = false; updates.call(); }
};
}
