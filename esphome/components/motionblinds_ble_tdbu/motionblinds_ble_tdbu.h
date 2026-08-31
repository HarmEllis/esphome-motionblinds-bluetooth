#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/components/motionblinds_ble/motionblinds_ble.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "motionblinds_tdbu_dispatch.h"
#include "motionblinds_tdbu_geometry.h"

namespace esphome::motionblinds_ble_tdbu {

using esphome::motionblinds_ble::MotionblindsBLEMotor;

/* Coordinates the two motors of one top-down bottom-up blind.
 *
 * Neither motor knows the other exists, so everything that keeps them from
 * driving into each other lives here. Five rules, in the order they apply:
 *
 *  1. Never move on a remembered position. A guided move needs a position that
 *     was observed during the current connection, from both motors.
 *  2. The rail that opens the gap goes first; the one closing it waits for
 *     observed evidence that there is room. A completed write is not evidence.
 *  3. Targets carry a safety margin on top of the physical clearance, because
 *     position feedback is whole-numbered and the motors' overshoot is not
 *     specified anywhere.
 *  4. If the first rail fails or never makes room, the second one does not
 *     move at all.
 *  5. While anything is moving, a watchdog stops both rails the moment the
 *     observed gap drops below the minimum. This is the only thing covering
 *     moves it did not plan: a stalling rail, the physical remote, or the
 *     favorite button whose target is unknowable.
 */
class MotionblindsBLETdbu : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_top_motor(MotionblindsBLEMotor *motor) { this->top_ = motor; }
  void set_bottom_motor(MotionblindsBLEMotor *motor) { this->bottom_ = motor; }
  void set_fabric(Fabric fabric) { this->fabric_ = fabric; }
  void set_min_gap(float min_gap) { this->min_gap_ = min_gap; }
  void set_safety_margin(float margin) { this->safety_margin_ = margin; }
  /// Observed gap below which two rails may not be started at the same moment.
  void set_start_gap(float gap) { this->start_gap_ = gap; }
  /// Let the start rule take the rails' direction of travel into account.
  /// The escape hatch for a blind on which the early release misbehaves: false
  /// restores the gap-only rule exactly. See motionblinds_tdbu_dispatch.h.
  void set_direction_aware(bool enabled) { this->direction_aware_ = enabled; }
  /// Trust the remembered rail positions instead of re-reading them from both
  /// motors before every move. See the README: this trades correctness under a
  /// physical remote for a very large reduction in latency.
  void set_optimistic(bool optimistic) { this->optimistic_ = optimistic; }
  /// Connect the rail that will move second while the first rail is opening
  /// clearance. Its movement command is still held until the measured gap is
  /// safe; only the otherwise-serial BLE setup is overlapped.
  void set_preconnect_trailing(bool enabled) { this->preconnect_trailing_ = enabled; }
  /// How long prepare() keeps both motor links warm while idle.
  void set_prepare_timeout(uint32_t ms) { this->prepare_timeout_ = ms; }
  void set_clearance_timeout(uint32_t ms) { this->clearance_timeout_ = ms; }
  void set_lease_timeout(uint32_t ms) { this->lease_timeout_ = ms; }

  Fabric fabric() const { return this->fabric_; }
  const Geometry &geometry() const { return this->geometry_; }

  // ------------------------------------------------------------- commands
  /// Move one rail to a Home Assistant cover position (0-1), addressing the
  /// window rather than the travel the other rail happens to leave.
  void set_rail_position(Rail rail, float position);
  /// Move the blind as a whole.
  void set_combined_openness(float openness);
  /// Slide the fabric block without changing how much of it is showing.
  void set_fabric_centre(float centre);
  void stop_rail(Rail rail);
  void stop_all();
  /// Warm both links before a time-critical command. Intended for an HA
  /// automation shortly before a scheduled move; automatically expires.
  void prepare();

  // ---------------------------------------------------------------- state
  /// The motor driving one rail, for entities that report per-motor state.
  MotionblindsBLEMotor *motor(Rail rail) const { return this->motor_(rail); }

  bool positions_known() const;
  float rail_position(Rail rail) const;
  float combined_openness() const;
  /// Where the middle of the segment between the rails currently sits.
  float fabric_centre() const;
  bool is_moving() const;
  /// Plain-language description of what the blind is doing, or why the last
  /// move did not happen. Surfaced as a diagnostic text sensor because "nothing
  /// moved and nothing was reported" is the failure this component exists to
  /// eliminate.
  const char *status_text() const;
  /// Which way the blind as a whole is heading: -1 opening, +1 closing, 0 idle.
  int8_t travel_direction() const;

  void add_on_update_callback(std::function<void()> &&callback) { this->update_callback_.add(std::move(callback)); }

 protected:
  /// What the coordinator is waiting for before the trailing rail may start.
  enum class Wait : uint8_t {
    NONE,       ///< there is already room; both may run together
    DEPARTURE,  ///< wait for observed proof that the leading rail has left
    CLEARANCE,  ///< wait until the leading rail has opened the gap up
  };

  enum class Phase : uint8_t {
    IDLE,
    REFRESHING,  ///< both motors are being asked where they are
    LEADING,     ///< the first rail is on its way
    TRAILING,    ///< the second rail is on its way
  };

  /// One user request, resolved into rail targets only at dispatch time.
  ///
  /// Requests are held as intent rather than as computed targets because the
  /// geometry can change while a request waits, and a target derived from a
  /// stale observation is exactly what this class exists to prevent.
  struct Intent {
    bool active{false};
    bool combined{false};
    bool has_top{false};
    bool has_bottom{false};
    float top_position{0.0f};  ///< requested Home Assistant cover position
    float bottom_position{0.0f};
    float length{0.0f};  ///< desired distance between the rails
    float centre{0.0f};  ///< desired midpoint between the rails
    bool has_length{false};
    bool has_centre{false};
    uint32_t generation{0};
    uint32_t submitted_at{0};
  };

  MotionblindsBLEMotor *motor_(Rail rail) const { return rail == Rail::TOP ? this->top_ : this->bottom_; }
  /// Whether either motor still owes us a command outcome. Distinct from
  /// is_moving(), which also reports the coordinator's own phase: waiting on
  /// that would mean waiting on ourselves and never finishing.
  bool motors_busy_() const;
  float position_(Rail rail) const;
  bool fresh_(Rail rail) const;

  void submit_(const Intent &intent);
  void begin_();
  void plan_and_dispatch_();
  /// Send one rail to a window target.
  ///
  /// `other_reference` is the position the target is kept clear of, and it is
  /// an explicit argument because the safe reference differs per phase. A
  /// follower normally uses the other rail's observed position; only two rails
  /// moving away may use the committed target because every interleaving then
  /// increases the gap. A bounded residual command completes targets that had
  /// to be clipped against an observed intermediate position.
  bool command_(Rail rail, float window_target, float other_reference);
  /// Whether both motors have reported a speed setting and the two differ. A
  /// trailing rail travelling faster than the one it follows closes the gap for
  /// the whole move, which no amount of departure proof can see coming.
  bool speeds_known_different_() const;
  bool departure_proven_() const;
  enum class ResidualResult : uint8_t { NONE, STARTED, FAILED };
  /// Re-issue rails whose commanded target had to be cut short, once the
  /// geometry allows the rest of it. Bounded to one extra command per rail.
  ResidualResult complete_residual_();
  void advance_();
  void finish_();
  void abandon_(const char *reason);
  void acquire_lease_(Rail rail);
  void release_leases_();
  void check_clearance_();
  void on_motor_update_();

  MotionblindsBLEMotor *top_{nullptr};
  MotionblindsBLEMotor *bottom_{nullptr};

  Fabric fabric_{Fabric::BETWEEN_RAILS};
  float min_gap_{0.0f};
  float safety_margin_{0.0f};
  float start_gap_{10.0f};
  bool direction_aware_{true};
  bool optimistic_{false};
  bool preconnect_trailing_{true};
  /// HA emits two cover actions as separate API messages. This delay is far
  /// below BLE connection time but long enough to combine those messages into
  /// one collision-aware plan. A genuinely single-rail command pays 30 ms.
  static constexpr uint32_t RAIL_PAIR_WINDOW_MS = 30;
  uint32_t prepare_timeout_{120000};
  uint32_t clearance_timeout_{60000};
  uint32_t lease_timeout_{180000};
  Geometry geometry_{};

  // The desired geometry is kept as one coupled pair. The combined cover and
  // the fabric-position number each change one half of it; holding them
  // separately would let the second request recompute its target from the
  // half the first had not applied yet, and undo it.
  float desired_length_{0.0f};
  float desired_centre_{50.0f};
  bool has_desired_{false};

  Intent pending_{};
  uint32_t generation_{0};

  Phase phase_{Phase::IDLE};
  uint32_t phase_since_{0};
  uint32_t operation_since_{0};
  uint32_t current_request_at_{0};
  bool leased_top_{false};
  bool leased_bottom_{false};
  bool prepared_{false};
  uint32_t prepared_since_{0};
  const char *last_error_{nullptr};

  Rail lead_{Rail::TOP};
  Rail trail_{Rail::TOP};
  bool has_trail_{false};
  float lead_target_{0.0f};
  float trail_target_{0.0f};
  Wait wait_{Wait::NONE};

  /// Where the leading rail was when its command was accepted, and whether
  /// that was an observation rather than a remembered position. A remembered
  /// baseline that turns out to be wrong looks exactly like movement, so it is
  /// re-baselined on the first real frame and never counted as proof.
  float lead_reference_{NAN};
  bool lead_reference_fresh_{false};
  uint32_t lead_command_at_{0};
  /// One raw step of the leading rail's own range: the smallest movement its
  /// whole-numbered feedback can possibly show.
  float departure_step_{1.0f};
  /// What the leading rail was actually commanded to. Used to recognise a
  /// settled lead and, only when both rails move away, as a safe clamp reference
  /// for the second command.
  float lead_committed_{0.0f};

  /// What each rail was asked for against what it could be given, so a target
  /// the geometry had to cut short can be finished off afterwards.
  struct RailCommand {
    bool active{false};
    bool residual_attempted{false};
    float intended{0.0f};
    float achieved{0.0f};
  };
  RailCommand commanded_[2]{};

  CallbackManager<void()> update_callback_{};
};

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
