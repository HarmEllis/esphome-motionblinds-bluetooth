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

const char *MotionblindsBLETdbu::status_text() const {
  if (this->last_error_ != nullptr && this->phase_ == Phase::IDLE)
    return this->last_error_;
  switch (this->phase_) {
    case Phase::REFRESHING:
      return "checking where both rails are";
    case Phase::LEADING:
      return "moving the first rail clear";
    case Phase::TRAILING:
      return "moving";
    case Phase::IDLE:
    default:
      return this->positions_known() ? "idle" : "position unknown";
  }
}

void MotionblindsBLETdbu::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE top-down bottom-up");
  ESP_LOGCONFIG(TAG,
                "  Fabric: %s\n"
                "  Minimum gap: %.0f%%\n"
                "  Safety margin: %.0f%%\n"
                "  Start gap: %.0f%%\n"
                "  Reachable segment: %.0f%% - %.0f%%",
                this->fabric_ == Fabric::BETWEEN_RAILS ? "between rails" : "outside in", this->min_gap_,
                this->safety_margin_, this->start_gap_, this->geometry_.min_length(),
                this->geometry_.max_length());
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

float MotionblindsBLETdbu::rail_position(Rail rail) const {
  const float window = this->position_(rail);
  if (std::isnan(window))
    return NAN;
  // Only this rail's own position is needed, so it stays stable when the other
  // one moves.
  return this->geometry_.rail_position(rail, window);
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

void MotionblindsBLETdbu::set_rail_position(Rail rail, float position) {
  Intent intent;
  intent.active = true;
  intent.combined = false;
  intent.rail = rail;
  intent.length = position;  // resolved against live geometry at dispatch
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
  this->last_error_ = nullptr;

  // Optimistic: the remembered positions are taken as true, so a move can be
  // planned and sent straight away. Only a rail that has never reported at all
  // still has to be read first — there is nothing to be optimistic about.
  if (this->optimistic_ && this->positions_known()) {
    this->plan_and_dispatch_();
    return;
  }

  // Otherwise a remembered position is not evidence of where a rail is now: a
  // remote or the vendor app can move one while this node is disconnected. Both
  // motors are read before anything is planned, which means waking both and
  // waiting for each to connect in turn.
  ESP_LOGD(TAG, "Refreshing both rail positions before moving");
  this->acquire_lease_(Rail::TOP);
  this->acquire_lease_(Rail::BOTTOM);
  this->top_->request_status();
  this->bottom_->request_status();
  this->phase_ = Phase::REFRESHING;
  this->phase_since_ = millis();
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
    // Clamp here rather than at dispatch, so the direction classification and
    // the clearance wait both reason about the target the rail will really be
    // given. A wait on an unreachable target would never be satisfied.
    target_top = this->geometry_.clamp_target(Rail::TOP, this->geometry_.rail_window_target(Rail::TOP, this->pending_.length), bottom);
  } else {
    target_bottom = this->geometry_.clamp_target(Rail::BOTTOM, this->geometry_.rail_window_target(Rail::BOTTOM, this->pending_.length), top);
  }

  const Direction top_direction = Geometry::classify(Rail::TOP, top, target_top);
  const Direction bottom_direction = Geometry::classify(Rail::BOTTOM, bottom, target_bottom);

  if (top_direction == Direction::STATIONARY && bottom_direction == Direction::STATIONARY) {
    ESP_LOGD(TAG, "Already there");
    this->finish_();
    return;
  }

  // Which rail leads is decided by what each one does to the gap: the rail that
  // opens it goes first. Whether the second has to wait is a separate question,
  // decided by how much room there is *right now* — two rails far apart can
  // start together even when one is closing on the other, while two rails
  // almost touching must not start together in any direction.
  const float gap = bottom - top;
  const bool roomy = gap >= this->start_gap_;

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
  } else {
    // Both rails move. The one whose travel opens the gap leads; if neither
    // does (both closing in) the top rail leads by convention, and the wait
    // below is what keeps them apart.
    const bool top_leads = top_direction == Direction::AWAY || bottom_direction != Direction::AWAY;
    this->lead_ = top_leads ? Rail::TOP : Rail::BOTTOM;
    this->trail_ = top_leads ? Rail::BOTTOM : Rail::TOP;
    this->lead_target_ = top_leads ? target_top : target_bottom;
    this->trail_target_ = top_leads ? target_bottom : target_top;
    this->has_trail_ = true;
    this->wait_ = roomy ? Wait::NONE : Wait::CLEARANCE;
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

  // Only a rail that is actually being commanded needs its connection held.
  this->acquire_lease_(rail);

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

  // Start the second rail once the leading one has actually opened the gap up,
  // not once it has finished. They are allowed to travel together; what they
  // must not do is set off together while they are close.
  const float gap = this->position_(Rail::BOTTOM) - this->position_(Rail::TOP);
  const bool settled = !this->motor_(this->lead_)->is_moving() &&
                       std::fabs(lead_position - this->lead_target_) <= 1.0f;
  if (gap < this->start_gap_ && !settled)
    return;

  ESP_LOGD(TAG, "First rail has opened a %.0f%% gap, starting the second", gap);
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
  ESP_LOGE(TAG, "Move abandoned: %s", reason);
  this->last_error_ = reason;
  this->finish_();
}

// ---------------------------------------------------------------- leases

void MotionblindsBLETdbu::acquire_lease_(Rail rail) {
  // Hold the connection open for the whole operation: without it the idle timer
  // can disconnect a motor while its rail is still travelling, ending the
  // position updates the clearance watchdog depends on.
  //
  // Taken per rail rather than for both, because a lease wakes a motor. Leasing
  // the rail that is not going to move meant every move woke two motors and,
  // since the tracker connects one client at a time, waited out two connections
  // to move one rail.
  bool &held = rail == Rail::TOP ? this->leased_top_ : this->leased_bottom_;
  if (held)
    return;
  this->motor_(rail)->acquire_lease();
  held = true;
}

void MotionblindsBLETdbu::release_leases_() {
  if (this->leased_top_) {
    this->top_->release_lease();
    this->leased_top_ = false;
  }
  if (this->leased_bottom_) {
    this->bottom_->release_lease();
    this->leased_bottom_ = false;
  }
}

// -------------------------------------------------------------- watchdog

void MotionblindsBLETdbu::check_clearance_() {
  if (!this->fresh_(Rail::TOP) || !this->fresh_(Rail::BOTTOM))
    return;

  const float gap = this->position_(Rail::BOTTOM) - this->position_(Rail::TOP);
  if (gap >= this->min_gap_)
    return;

  // Only a real breach counts. On most of these blinds the rails stack against
  // each other perfectly happily, so min_gap defaults to zero and this fires
  // only when the bottom rail has ended up above the top one.
  ESP_LOGE(TAG, "Rails are %.1f%% apart, closer than the %.0f%% allowed; stopping both", gap, this->min_gap_);
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
        break;
      }
      // Both rail positions are needed before anything may move. If one motor
      // has already given up there is nothing left to wait for, and sitting out
      // the full clearance timeout only hides which rail is the problem.
      for (const Rail rail : {Rail::TOP, Rail::BOTTOM}) {
        MotionblindsBLEMotor *motor = this->motor_(rail);
        if (motor != nullptr && motor->state() == esphome::motionblinds_ble::MotorState::FAILED) {
          this->abandon_(rail == Rail::TOP ? "the top rail could not be reached, so the bottom rail was not moved"
                                           : "the bottom rail could not be reached, so the top rail was not moved");
          return;
        }
      }
      if (now - this->phase_since_ > this->clearance_timeout_)
        this->abandon_("could not establish where both rails are");
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
