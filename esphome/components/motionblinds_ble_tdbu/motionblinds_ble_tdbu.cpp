#include "motionblinds_ble_tdbu.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::motionblinds_ble_tdbu {

static const char *const TAG = "motionblinds_ble_tdbu";

void MotionblindsBLETdbu::setup() {
  this->geometry_ = Geometry(this->fabric_, this->min_gap_, this->safety_margin_, this->top_->rail_range(),
                             this->bottom_->rail_range());

  if (!this->geometry_.valid()) {
    ESP_LOGE(TAG, "The two rails cannot keep %.0f%% apart within their travel ranges", this->min_gap_);
    this->mark_failed();
    return;
  }

  auto on_update = [this]() { this->on_motor_update_(); };
  this->top_->add_on_update_callback(on_update);
  this->bottom_->add_on_update_callback(on_update);
}

void MotionblindsBLETdbu::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE top-down bottom-up");
  ESP_LOGCONFIG(TAG,
                "  Fabric: %s\n"
                "  Minimum gap: %.0f%%\n"
                "  Safety margin: %.0f%%\n"
                "  Reachable segment: %.0f%% - %.0f%%",
                this->fabric_ == Fabric::BETWEEN_RAILS ? "between rails" : "outside in", this->min_gap_,
                this->safety_margin_, this->geometry_.min_length(), this->geometry_.max_length());
}

// ------------------------------------------------------------------ state

float MotionblindsBLETdbu::position_(Rail rail) const {
  const MotionblindsBLEMotor *motor = this->motor_(rail);
  return motor == nullptr ? NAN : motor->window_position();
}

bool MotionblindsBLETdbu::fresh_(Rail rail) const {
  const MotionblindsBLEMotor *motor = this->motor_(rail);
  return motor != nullptr && motor->position_fresh() && !std::isnan(motor->window_position());
}

bool MotionblindsBLETdbu::positions_known() const {
  return !std::isnan(this->position_(Rail::TOP)) && !std::isnan(this->position_(Rail::BOTTOM));
}

float MotionblindsBLETdbu::rail_openness(Rail rail) const {
  if (!this->positions_known())
    return NAN;
  const Rail other = rail == Rail::TOP ? Rail::BOTTOM : Rail::TOP;
  return this->geometry_.rail_openness(rail, this->position_(rail), this->position_(other));
}

float MotionblindsBLETdbu::combined_openness() const {
  if (!this->positions_known())
    return NAN;
  return this->geometry_.length_to_openness(this->position_(Rail::BOTTOM) - this->position_(Rail::TOP));
}

float MotionblindsBLETdbu::fabric_centre() const {
  if (!this->positions_known())
    return NAN;
  return (this->position_(Rail::TOP) + this->position_(Rail::BOTTOM)) / 2.0f;
}

bool MotionblindsBLETdbu::is_moving() const {
  return this->phase_ == Phase::LEADING || this->phase_ == Phase::TRAILING || this->motors_busy_();
}

bool MotionblindsBLETdbu::motors_busy_() const {
  return (this->top_ != nullptr && this->top_->busy()) || (this->bottom_ != nullptr && this->bottom_->busy());
}

int8_t MotionblindsBLETdbu::travel_direction() const {
  // The rails move in opposite senses, so the blind's direction is whichever
  // one is shrinking or growing the segment between them, mapped through the
  // fabric to openness.
  int8_t segment = 0;
  if (this->top_ != nullptr && this->top_->travel_direction() != 0)
    segment = this->top_->travel_direction() > 0 ? -1 : 1;
  else if (this->bottom_ != nullptr && this->bottom_->travel_direction() != 0)
    segment = this->bottom_->travel_direction() > 0 ? 1 : -1;
  if (segment == 0)
    return 0;
  // A growing segment opens an outside_in blind and closes a between_rails one.
  return this->fabric_ == Fabric::OUTSIDE_IN ? static_cast<int8_t>(-segment) : segment;
}

// --------------------------------------------------------------- requests

void MotionblindsBLETdbu::set_rail_openness(Rail rail, float openness) {
  Intent intent;
  intent.active = true;
  intent.combined = false;
  intent.rail = rail;
  intent.length = openness;  // resolved against live geometry at dispatch
  this->submit_(intent);
}

void MotionblindsBLETdbu::set_combined_openness(float openness) {
  Intent intent;
  intent.active = true;
  intent.combined = true;
  intent.length = this->geometry_.openness_to_length(openness);
  intent.has_length = true;
  // The centre is deliberately left to whatever the last request set, so that
  // changing how much fabric shows does not silently move it as well.
  intent.has_centre = false;
  this->submit_(intent);
}

void MotionblindsBLETdbu::set_fabric_centre(float centre) {
  Intent intent;
  intent.active = true;
  intent.combined = true;
  intent.centre = centre;
  intent.has_centre = true;
  intent.has_length = false;
  this->submit_(intent);
}

void MotionblindsBLETdbu::submit_(const Intent &intent) {
  if (this->is_failed())
    return;

  // Fold the request into the single desired geometry. Two entities each own
  // one half of it, and merging here is what stops the second from computing
  // its target from the half the first has not applied yet.
  if (intent.combined) {
    if (intent.has_length)
      this->desired_length_ = intent.length;
    if (intent.has_centre)
      this->desired_centre_ = intent.centre;
    if (!this->has_desired_) {
      if (!intent.has_length)
        this->desired_length_ = this->positions_known()
                                    ? this->position_(Rail::BOTTOM) - this->position_(Rail::TOP)
                                    : this->geometry_.min_length();
      if (!intent.has_centre)
        this->desired_centre_ = this->positions_known() ? this->fabric_centre() : 50.0f;
      this->has_desired_ = true;
    }
  }

  this->pending_ = intent;
  this->pending_.generation = ++this->generation_;

  if (this->phase_ == Phase::IDLE)
    this->begin_();
}

void MotionblindsBLETdbu::stop_rail(Rail rail) {
  // Stop never queues and never waits for the geometry: it has to work while a
  // move is in flight, and it can only ever make the rails safer.
  MotionblindsBLEMotor *motor = this->motor_(rail);
  if (motor != nullptr)
    motor->request_stop();
  this->pending_ = Intent{};
  if (this->phase_ != Phase::IDLE)
    this->abandon_("stopped");
}

void MotionblindsBLETdbu::stop_all() {
  if (this->top_ != nullptr)
    this->top_->request_stop();
  if (this->bottom_ != nullptr)
    this->bottom_->request_stop();
  this->pending_ = Intent{};
  if (this->phase_ != Phase::IDLE)
    this->abandon_("stopped");
}

// ------------------------------------------------------------- the moving

void MotionblindsBLETdbu::begin_() {
  this->operation_since_ = millis();
  this->acquire_leases_();

  // A remembered position is not evidence of where a rail is now: a remote or
  // the vendor app can move one while this node is disconnected. Ask both
  // motors before planning anything.
  if (!this->fresh_(Rail::TOP) || !this->fresh_(Rail::BOTTOM)) {
    ESP_LOGD(TAG, "Refreshing both rail positions before moving");
    this->top_->request_status();
    this->bottom_->request_status();
    this->phase_ = Phase::REFRESHING;
    this->phase_since_ = millis();
    return;
  }

  this->plan_and_dispatch_();
}

void MotionblindsBLETdbu::plan_and_dispatch_() {
  const float top = this->position_(Rail::TOP);
  const float bottom = this->position_(Rail::BOTTOM);

  float target_top = top;
  float target_bottom = bottom;

  if (this->pending_.combined) {
    const Placement placement = this->geometry_.place_segment(this->desired_length_, this->desired_centre_);
    if (!placement.feasible)
      ESP_LOGW(TAG, "Requested %.0f%% of travel is not reachable; using %.0f%%", this->desired_length_,
               placement.length);
    target_top = placement.top;
    target_bottom = placement.bottom;
  } else if (this->pending_.rail == Rail::TOP) {
    target_top = this->geometry_.rail_target(Rail::TOP, this->pending_.length, bottom);
  } else {
    target_bottom = this->geometry_.rail_target(Rail::BOTTOM, this->pending_.length, top);
  }

  const Direction top_direction = Geometry::classify(Rail::TOP, top, target_top);
  const Direction bottom_direction = Geometry::classify(Rail::BOTTOM, bottom, target_bottom);

  if (top_direction == Direction::STATIONARY && bottom_direction == Direction::STATIONARY) {
    ESP_LOGD(TAG, "Already there");
    this->finish_();
    return;
  }

  // A rail moving toward the other one may only start once there is observed
  // room for it, so the rail that makes room has to go first.
  if (top_direction == Direction::STATIONARY) {
    this->lead_ = Rail::BOTTOM;
    this->lead_target_ = target_bottom;
    this->has_trail_ = false;
    this->wait_ = Wait::NONE;
  } else if (bottom_direction == Direction::STATIONARY) {
    this->lead_ = Rail::TOP;
    this->lead_target_ = target_top;
    this->has_trail_ = false;
    this->wait_ = Wait::NONE;
  } else if (top_direction == Direction::AWAY && bottom_direction == Direction::AWAY) {
    // Both widening the gap: neither can run into the other, so they go together.
    this->lead_ = Rail::TOP;
    this->lead_target_ = target_top;
    this->trail_ = Rail::BOTTOM;
    this->trail_target_ = target_bottom;
    this->has_trail_ = true;
    this->wait_ = Wait::NONE;
  } else if (top_direction == Direction::AWAY) {
    this->lead_ = Rail::TOP;
    this->lead_target_ = target_top;
    this->trail_ = Rail::BOTTOM;
    this->trail_target_ = target_bottom;
    this->has_trail_ = true;
    this->wait_ = Wait::CLEARANCE;
  } else if (bottom_direction == Direction::AWAY) {
    this->lead_ = Rail::BOTTOM;
    this->lead_target_ = target_bottom;
    this->trail_ = Rail::TOP;
    this->trail_target_ = target_top;
    this->has_trail_ = true;
    this->wait_ = Wait::CLEARANCE;
  } else {
    // Both closing in. The end state is safe because the targets already keep
    // their distance, but only if the rails do not travel at the same time:
    // the second one waits for the first to actually arrive, not merely to
    // have started.
    this->lead_ = Rail::TOP;
    this->lead_target_ = target_top;
    this->trail_ = Rail::BOTTOM;
    this->trail_target_ = target_bottom;
    this->has_trail_ = true;
    this->wait_ = Wait::SETTLED;
  }

  if (!this->command_(this->lead_, this->lead_target_)) {
    this->abandon_("the first rail did not accept its command");
    return;
  }

  if (this->has_trail_ && this->wait_ == Wait::NONE) {
    if (!this->command_(this->trail_, this->trail_target_)) {
      this->abandon_("the second rail did not accept its command");
      return;
    }
    this->has_trail_ = false;
  }

  this->phase_ = this->has_trail_ ? Phase::LEADING : Phase::TRAILING;
  this->phase_since_ = millis();
  this->pending_.active = false;
}

bool MotionblindsBLETdbu::command_(Rail rail, float window_target) {
  MotionblindsBLEMotor *motor = this->motor_(rail);
  if (motor == nullptr)
    return false;

  const Rail other = rail == Rail::TOP ? Rail::BOTTOM : Rail::TOP;
  const uint8_t raw = this->geometry_.raw_target(rail, window_target, this->position_(other));
  const float achieved = motor->rail_range().to_window(static_cast<float>(raw));

  ESP_LOGD(TAG, "Commanding %s rail to %.1f%% of the window (raw %u)", rail == Rail::TOP ? "top" : "bottom", achieved,
           static_cast<unsigned>(raw));
  return motor->request_position(achieved);
}

void MotionblindsBLETdbu::advance_() {
  if (this->phase_ != Phase::LEADING || !this->has_trail_)
    return;

  const float lead_position = this->position_(this->lead_);
  if (std::isnan(lead_position))
    return;

  bool ready = false;
  if (this->wait_ == Wait::CLEARANCE) {
    // Enough room exists once the leading rail has passed the point where the
    // trailing rail's target still clears it.
    const float required = this->geometry_.effective_gap();
    ready = this->trail_ == Rail::TOP ? (lead_position - this->trail_target_) >= required
                                      : (this->trail_target_ - lead_position) >= required;
  } else if (this->wait_ == Wait::SETTLED) {
    ready = !this->motor_(this->lead_)->is_moving() && std::fabs(lead_position - this->lead_target_) <= 1.0f;
  }

  if (!ready)
    return;

  ESP_LOGD(TAG, "First rail has made room, starting the second");
  if (!this->command_(this->trail_, this->trail_target_)) {
    this->abandon_("the second rail did not accept its command");
    return;
  }
  this->has_trail_ = false;
  this->phase_ = Phase::TRAILING;
  this->phase_since_ = millis();
}

void MotionblindsBLETdbu::finish_() {
  this->phase_ = Phase::IDLE;
  this->has_trail_ = false;
  this->wait_ = Wait::NONE;
  this->operation_since_ = 0;
  this->release_leases_();
  this->update_callback_.call();
}

void MotionblindsBLETdbu::abandon_(const char *reason) {
  ESP_LOGE(TAG, "Move abandoned: %s. The other rail was not moved.", reason);
  this->finish_();
}

// ---------------------------------------------------------------- leases

void MotionblindsBLETdbu::acquire_leases_() {
  if (this->leases_held_)
    return;
  // Hold both connections open for the whole operation. Without this the idle
  // timer can disconnect a motor while its rail is still travelling, which
  // would end the position updates the clearance watchdog depends on.
  this->top_->acquire_lease();
  this->bottom_->acquire_lease();
  this->leases_held_ = true;
}

void MotionblindsBLETdbu::release_leases_() {
  if (!this->leases_held_)
    return;
  this->top_->release_lease();
  this->bottom_->release_lease();
  this->leases_held_ = false;
}

// -------------------------------------------------------------- watchdog

void MotionblindsBLETdbu::check_clearance_() {
  if (!this->fresh_(Rail::TOP) || !this->fresh_(Rail::BOTTOM))
    return;

  const float gap = this->position_(Rail::BOTTOM) - this->position_(Rail::TOP);
  if (gap >= this->min_gap_)
    return;

  // Reactive by nature: this sees a breach only once it has happened, which is
  // why the targets carry a safety margin. It still covers everything the
  // planning cannot, including moves this component never issued.
  ESP_LOGE(TAG, "Rails are only %.1f%% apart, stopping both", gap);
  this->top_->request_stop();
  this->bottom_->request_stop();
  if (this->phase_ != Phase::IDLE)
    this->abandon_("rails came too close");
}

void MotionblindsBLETdbu::on_motor_update_() {
  this->check_clearance_();
  this->update_callback_.call();
}

// ------------------------------------------------------------------ loop

void MotionblindsBLETdbu::loop() {
  if (this->phase_ == Phase::IDLE) {
    if (this->pending_.active)
      this->begin_();
    return;
  }

  const uint32_t now = millis();

  switch (this->phase_) {
    case Phase::REFRESHING:
      if (this->fresh_(Rail::TOP) && this->fresh_(Rail::BOTTOM)) {
        this->plan_and_dispatch_();
      } else if (now - this->phase_since_ > this->clearance_timeout_) {
        this->abandon_("could not establish where both rails are");
      }
      break;

    case Phase::LEADING:
      this->advance_();
      if (this->phase_ == Phase::LEADING && now - this->phase_since_ > this->clearance_timeout_)
        this->abandon_("the first rail never made room");
      break;

    case Phase::TRAILING:
      // Wait on the motors, not on is_moving(): that reports this phase too,
      // so the condition could never come true and every move would run until
      // the lease expired.
      if (!this->motors_busy_())
        this->finish_();
      break;

    default:
      break;
  }

  // A lease may never become a new way to hang.
  if (this->operation_since_ != 0 && now - this->operation_since_ > this->lease_timeout_)
    this->abandon_("operation exceeded its total deadline");
}

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
