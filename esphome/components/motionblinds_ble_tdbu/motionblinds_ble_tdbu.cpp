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
  if (this->prepared_ && this->phase_ == Phase::IDLE) {
    if (this->fresh_(Rail::TOP) && this->fresh_(Rail::BOTTOM) &&
        this->top_->state() == esphome::motionblinds_ble::MotorState::READY &&
        this->bottom_->state() == esphome::motionblinds_ble::MotorState::READY)
      return "ready for immediate command";
    return "preparing both rails";
  }
  switch (this->phase_) {
    case Phase::REFRESHING:
      return "checking where both rails are";
    case Phase::LEADING:
      return this->wait_ == Wait::DEPARTURE ? "waiting for the first rail to move off"
                                            : "moving the first rail clear";
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
                "  Direction aware: %s\n"
                "  Preconnect trailing rail: %s\n"
                "  Prepare timeout: %us\n"
                "  Reachable segment: %.0f%% - %.0f%%",
                this->fabric_ == Fabric::BETWEEN_RAILS ? "between rails" : "outside in", this->min_gap_,
                this->safety_margin_, this->start_gap_, YESNO(this->direction_aware_),
                YESNO(this->preconnect_trailing_), static_cast<unsigned>(this->prepare_timeout_ / 1000),
                this->geometry_.min_length(), this->geometry_.max_length());
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
  return this->phase_ == Phase::LEADING || this->phase_ == Phase::TRAILING ||
         (this->top_ != nullptr && this->top_->is_moving()) ||
         (this->bottom_ != nullptr && this->bottom_->is_moving());
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
  intent.has_top = rail == Rail::TOP;
  intent.has_bottom = rail == Rail::BOTTOM;
  if (intent.has_top)
    intent.top_position = position;
  else
    intent.bottom_position = position;
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

  const uint32_t now = millis();

  // Two direct rail entities describe one desired geometry but Home Assistant
  // transports them as separate API calls. Preserve both when they overlap in
  // the pending slot; otherwise the second call replaces the first before the
  // coordinator can choose the rail that opens the gap.
  if (!intent.combined && this->pending_.active && !this->pending_.combined) {
    const bool already_paired = this->pending_.has_top && this->pending_.has_bottom;
    if (intent.has_top) {
      this->pending_.has_top = true;
      this->pending_.top_position = intent.top_position;
    }
    if (intent.has_bottom) {
      this->pending_.has_bottom = true;
      this->pending_.bottom_position = intent.bottom_position;
    }
    this->pending_.generation = ++this->generation_;
    if (!already_paired && this->pending_.has_top && this->pending_.has_bottom)
      ESP_LOGI(TAG, "Combined top and bottom cover calls received %ums apart; planning them as one rail pair",
               static_cast<unsigned>(now - this->pending_.submitted_at));
  } else {
    this->pending_ = intent;
    this->pending_.generation = ++this->generation_;
    this->pending_.submitted_at = now;
  }

  if (this->phase_ == Phase::IDLE &&
      rail_request_ready(this->pending_.combined, this->pending_.has_top, this->pending_.has_bottom,
                         now - this->pending_.submitted_at, RAIL_PAIR_WINDOW_MS))
    this->begin_();
}

void MotionblindsBLETdbu::stop_rail(Rail rail) {
  // Stop never queues and never waits for the geometry: it has to work while a
  // move is in flight, and it can only ever make the rails safer.
  MotionblindsBLEMotor *motor = this->motor_(rail);
  if (motor != nullptr)
    motor->request_stop();
  this->pending_ = Intent{};
  if (this->phase_ != Phase::IDLE) {
    this->abandon_("stopped");
  } else if (this->prepared_) {
    this->prepared_ = false;
    this->prepared_since_ = 0;
    this->release_leases_();
    this->update_callback_.call();
  }
}

void MotionblindsBLETdbu::stop_all() {
  if (this->top_ != nullptr)
    this->top_->request_stop();
  if (this->bottom_ != nullptr)
    this->bottom_->request_stop();
  this->pending_ = Intent{};
  if (this->phase_ != Phase::IDLE) {
    this->abandon_("stopped");
  } else if (this->prepared_) {
    this->prepared_ = false;
    this->prepared_since_ = 0;
    this->release_leases_();
    this->update_callback_.call();
  }
}

void MotionblindsBLETdbu::prepare() {
  if (this->is_failed() || this->phase_ != Phase::IDLE)
    return;

  this->last_error_ = nullptr;
  this->prepared_ = true;
  this->prepared_since_ = millis();
  this->acquire_lease_(Rail::TOP);
  this->acquire_lease_(Rail::BOTTOM);

  // A lease wakes an idle motor and the normal handshake obtains a fresh
  // position. FAILED is the one state a lease deliberately does not restart;
  // a status request is the explicit retry in that case.
  if (this->top_->state() == esphome::motionblinds_ble::MotorState::FAILED)
    this->top_->request_status();
  if (this->bottom_->state() == esphome::motionblinds_ble::MotorState::FAILED)
    this->bottom_->request_status();

  ESP_LOGI(TAG, "Preparing both rail connections for the next command (expires in %us)",
           static_cast<unsigned>(this->prepare_timeout_ / 1000));
  this->update_callback_.call();
}

// ------------------------------------------------------------- the moving

void MotionblindsBLETdbu::begin_() {
  this->operation_since_ = millis();
  this->last_error_ = nullptr;
  this->prepared_ = false;
  this->prepared_since_ = 0;

  // A prepared or otherwise still-open pair has already supplied positions in
  // this connection. Even conservative mode gains nothing from asking both
  // motors the same question a second time.
  if (this->fresh_(Rail::TOP) && this->fresh_(Rail::BOTTOM)) {
    ESP_LOGD(TAG, "Both rail positions are already fresh; dispatching immediately");
    this->plan_and_dispatch_();
    return;
  }

  // Optimistic: the remembered positions are taken as true, so a move can be
  // planned and sent straight away. Only a rail that has never reported at all
  // still has to be read first — there is nothing to be optimistic about.
  if (this->optimistic_ && this->positions_known()) {
    this->plan_and_dispatch_();
    return;
  }

  // Otherwise a remembered position is not evidence of where a rail is now: a
  // remote or the vendor app can move one while this node is disconnected. A
  // rail already observed on its current link does not need the same query
  // twice, but both must be fresh before anything is planned.
  ESP_LOGD(TAG, "Refreshing stale rail positions before moving");
  this->acquire_lease_(Rail::TOP);
  this->acquire_lease_(Rail::BOTTOM);
  if (!this->fresh_(Rail::TOP))
    this->top_->request_status();
  if (!this->fresh_(Rail::BOTTOM))
    this->bottom_->request_status();
  this->phase_ = Phase::REFRESHING;
  this->phase_since_ = millis();
}

void MotionblindsBLETdbu::plan_and_dispatch_() {
  // Consume the request up front. Every early return below — already there, a
  // rail that would not take its command — used to leave it active, and the
  // idle branch of loop() would immediately start planning it again: a blind
  // already at its target woke both motors forever.
  //
  // A genuinely newer request submitted while this one runs replaces
  // pending_ with a later generation and is not lost by this.
  this->current_request_at_ = this->pending_.submitted_at;
  this->pending_.active = false;

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
  } else if (this->pending_.has_top && this->pending_.has_bottom) {
    // Resolve both absolute rail positions together. This is the path used by
    // parallel HA cover actions and is what lets the dispatcher see that, for
    // example, the bottom rail must leave before the top rail can follow it.
    const float requested_top = this->geometry_.rail_window_target(Rail::TOP, this->pending_.top_position);
    const float requested_bottom =
        this->geometry_.rail_window_target(Rail::BOTTOM, this->pending_.bottom_position);
    const Placement placement =
        this->geometry_.place_segment(requested_bottom - requested_top, (requested_top + requested_bottom) / 2.0f);
    if (!placement.feasible)
      ESP_LOGW(TAG, "Requested rail pair is not reachable without crossing; using the nearest safe placement");
    target_top = placement.top;
    target_bottom = placement.bottom;
  } else if (this->pending_.has_top) {
    // Clamp here rather than at dispatch, so the direction classification and
    // the clearance wait both reason about the target the rail will really be
    // given. A wait on an unreachable target would never be satisfied.
    target_top = this->geometry_.clamp_target(
        Rail::TOP, this->geometry_.rail_window_target(Rail::TOP, this->pending_.top_position), bottom);
  } else {
    target_bottom = this->geometry_.clamp_target(
        Rail::BOTTOM, this->geometry_.rail_window_target(Rail::BOTTOM, this->pending_.bottom_position), top);
  }

  const Direction top_direction = Geometry::classify(Rail::TOP, top, target_top);
  const Direction bottom_direction = Geometry::classify(Rail::BOTTOM, bottom, target_bottom);

  DispatchRequest request;
  request.top = top_direction;
  request.bottom = bottom_direction;
  request.gap = bottom - top;
  request.start_gap = this->start_gap_;
  request.direction_aware = this->direction_aware_;
  request.speeds_known_different = this->speeds_known_different_();

  const DispatchPlan plan = plan_dispatch(request);

  if (!plan.moves) {
    ESP_LOGD(TAG, "Already there");
    this->finish_();
    return;
  }

  this->lead_ = plan.lead;
  this->trail_ = plan.trail;
  this->has_trail_ = plan.has_trail;
  this->lead_target_ = plan.lead == Rail::TOP ? target_top : target_bottom;
  this->trail_target_ = plan.lead == Rail::TOP ? target_bottom : target_top;
  MotionblindsBLEMotor *lead_motor = this->motor_(plan.lead);
  if (lead_motor == nullptr) {
    this->abandon_("the leading rail is not configured");
    return;
  }
  this->departure_step_ = rail_quantum(lead_motor->rail_range());
  this->commanded_[0] = RailCommand{};
  this->commanded_[1] = RailCommand{};

  switch (plan.rule) {
    case StartRule::TOGETHER:
      this->wait_ = Wait::NONE;
      break;
    case StartRule::AFTER_DEPARTURE:
      this->wait_ = Wait::DEPARTURE;
      break;
    case StartRule::AFTER_CLEARANCE:
      this->wait_ = Wait::CLEARANCE;
      break;
  }

  if (plan.has_trail && plan.rule != StartRule::TOGETHER) {
    // Worth an INFO line: this is the difference between a move that feels
    // immediate and one that waits out a whole rail's travel, and which of the
    // two happened is otherwise invisible from Home Assistant.
    if (plan.same_direction && plan.rule == StartRule::AFTER_CLEARANCE && this->direction_aware_)
      ESP_LOGI(TAG, "Both rails travel the same way but their speed settings differ; keeping the conservative wait");
    ESP_LOGD(TAG, "%s rail leads; the %s rail waits for %s", plan.lead == Rail::TOP ? "Top" : "Bottom",
             plan.trail == Rail::TOP ? "top" : "bottom",
             plan.rule == StartRule::AFTER_DEPARTURE ? "it to move off" : "clearance");
  }

  // The rail that goes first is held off where the other one is standing: it is
  // the only reference that exists yet.
  if (!this->command_(this->lead_, this->lead_target_, this->position_(this->trail_))) {
    this->abandon_("the first rail did not accept its command");
    return;
  }

  if (this->has_trail_ && this->wait_ == Wait::NONE) {
    // The clamp reference for the follower depends on the geometry. Clamping
    // against the live position can temporarily truncate a simultaneous
    // translation, so the bounded residual pass below re-issues the remainder.
    // A committed target is a safe clamp reference only when both rails move
    // away and the gap therefore cannot shrink. In every other simultaneous
    // case use the observed position and let the bounded residual pass finish
    // any temporary truncation after the lead has actually arrived.
    const float trail_reference = plan.both_away ? this->lead_committed_ : this->position_(this->lead_);
    if (!this->command_(this->trail_, this->trail_target_, trail_reference)) {
      this->abandon_("the second rail did not accept its command");
      return;
    }
    this->has_trail_ = false;
  }

  this->phase_ = this->has_trail_ ? Phase::LEADING : Phase::TRAILING;
  this->phase_since_ = millis();
}

bool MotionblindsBLETdbu::speeds_known_different_() const {
  if (this->top_ == nullptr || this->bottom_ == nullptr)
    return false;
  const auto top_speed = this->top_->speed();
  const auto bottom_speed = this->bottom_->speed();
  // Unknown is not "different". Most motors never report a speed at all, and
  // refusing the fast path on missing information would mean never taking it.
  if (!top_speed.has_value() || !bottom_speed.has_value())
    return false;
  return *top_speed != *bottom_speed;
}

bool MotionblindsBLETdbu::command_(Rail rail, float window_target, float other_reference) {
  MotionblindsBLEMotor *motor = this->motor_(rail);
  if (motor == nullptr)
    return false;

  // Without a reference there is nothing to keep the target clear of, and an
  // unclamped absolute position is exactly what this class exists to prevent.
  // Unreachable from the planner, which requires both positions first; refused
  // rather than trusted, because the failure would be a collision.
  if (std::isnan(other_reference) || std::isnan(window_target)) {
    ESP_LOGE(TAG, "Refusing to command the %s rail: the other rail's position is unknown",
             rail == Rail::TOP ? "top" : "bottom");
    return false;
  }

  // Only a rail that is actually being commanded needs its connection held.
  this->acquire_lease_(rail);

  const uint8_t raw = this->geometry_.raw_target(rail, window_target, other_reference);
  const float achieved = motor->rail_range().to_window(static_cast<float>(raw));

  // Deliberately INFO: what was actually asked of a motor is the first thing
  // anyone needs when a blind does not move, and debug lines do not reach the
  // Home Assistant log.
  ESP_LOGI(TAG, "Commanding %s rail to %.1f%% of the window (raw %u)", rail == Rail::TOP ? "top" : "bottom", achieved,
           static_cast<unsigned>(raw));
  if (!motor->request_position(achieved, this->current_request_at_))
    return false;

  RailCommand &record = this->commanded_[rail == Rail::TOP ? 0 : 1];
  record.active = true;
  record.intended = window_target;
  record.achieved = achieved;

  if (rail == this->lead_) {
    this->lead_committed_ = achieved;
    this->lead_command_at_ = millis();
    // The baseline for departure proof, sampled where the lead was when its
    // command was accepted. Only an observed position may serve as proof; a
    // remembered one is re-baselined on the first real frame instead.
    this->lead_reference_ = this->position_(rail);
    this->lead_reference_fresh_ = this->fresh_(rail);
  }
  return true;
}

bool MotionblindsBLETdbu::departure_proven_() const {
  MotionblindsBLEMotor *lead = this->motor_(this->lead_);
  if (lead == nullptr)
    return false;

  DepartureEvidence evidence;
  evidence.lead = this->lead_;
  evidence.lead_fresh = this->fresh_(this->lead_);
  uint8_t raw_origin = 0;
  bool origin_fresh = false;
  const bool origin_known = lead->command_origin(raw_origin, origin_fresh);
  evidence.baseline_fresh = origin_known && origin_fresh;
  // A completed write, not a locally-set moving flag. The flag is set by this
  // node the moment it hands a command to the radio and says nothing about the
  // rail; this says the absolute position actually went out, which is what
  // makes subsequent feedback attributable to our own command.
  evidence.write_sent = lead->position_write_sent();
  // Prefer the position captured when the BLE write actually went out. The
  // coordinator may have queued it seconds earlier, during which a remote can
  // move the rail. Optimistic mode deliberately has no fresh write origin; its
  // first real frame is re-baselined below and used on the next frame instead.
  evidence.baseline = evidence.baseline_fresh
                          ? lead->rail_range().to_window(static_cast<float>(raw_origin))
                          : this->lead_reference_;
  if (!evidence.baseline_fresh && this->lead_reference_fresh_ && !std::isnan(this->lead_reference_))
    evidence.baseline_fresh = true;
  evidence.now = this->position_(this->lead_);
  evidence.quantum = this->departure_step_;
  evidence.gap = this->position_(Rail::BOTTOM) - this->position_(Rail::TOP);
  evidence.effective_gap = this->geometry_.effective_gap();
  return departure_proven(evidence);
}

void MotionblindsBLETdbu::advance_() {
  if (this->phase_ != Phase::LEADING || !this->has_trail_)
    return;

  // Do not let the tracker choose the trailing rail first: that would delay the
  // first physical movement, since it connects one client at a time. Once the
  // leading rail's command has actually gone out, the second connection can be
  // opened and keyed in parallel with the clearance move. No position command is
  // sent here.
  //
  // Gated on the write having left rather than on the lead's is_moving() flag.
  // The two become true at the same instant, so nothing is delayed by the
  // change; what it removes is a decision resting on a flag this node sets
  // about its own intent.
  const bool trailing_lease_held = this->trail_ == Rail::TOP ? this->leased_top_ : this->leased_bottom_;
  if (this->preconnect_trailing_ && !trailing_lease_held && this->motor_(this->lead_)->position_write_sent()) {
    ESP_LOGI(TAG, "First rail's command has gone out; preconnecting the second rail while clearance opens");
    this->acquire_lease_(this->trail_);
  }

  const float lead_position = this->position_(this->lead_);
  if (std::isnan(lead_position))
    return;

  // A baseline that was only remembered is replaced by the first observation,
  // and that observation is never itself counted as movement: a remembered
  // position that turns out to be wrong looks exactly like a rail departing.
  if (!this->lead_reference_fresh_ && this->fresh_(this->lead_)) {
    ESP_LOGD(TAG, "First observation of the leading rail at %.1f%%; using it as the departure baseline",
             lead_position);
    this->lead_reference_ = lead_position;
    this->lead_reference_fresh_ = true;
  }

  const float gap = this->position_(Rail::BOTTOM) - this->position_(Rail::TOP);

  // The leading rail is done, whichever wait is in force. Deliberately built on
  // the motor having no outstanding work plus a freshly observed position on
  // target: !is_moving() alone is true whenever a link drops mid-move, which is
  // exactly when the coordinator has lost its eyes.
  const bool settled = this->fresh_(this->lead_) && !this->motor_(this->lead_)->busy() &&
                       std::fabs(lead_position - this->lead_committed_) <= 1.0f;

  bool release = settled;
  const char *why = "has finished";
  if (!release && this->wait_ == Wait::DEPARTURE) {
    release = this->departure_proven_();
    why = "has moved off";
  } else if (!release && gap >= this->start_gap_) {
    release = true;
    why = "has opened the gap";
  }
  if (!release)
    return;

  // The evidence, at INFO, because the next field note about this depends on
  // being able to read the numbers the decision was actually made on.
  ESP_LOGI(TAG, "%s rail %s: observed at %.1f%% (was %.1f%%) after %.1fs, gap %.1f%%; starting the %s rail",
           this->lead_ == Rail::TOP ? "Top" : "Bottom", why, lead_position,
           std::isnan(this->lead_reference_) ? lead_position : this->lead_reference_,
           static_cast<double>(millis() - this->lead_command_at_) / 1000.0, gap,
           this->trail_ == Rail::TOP ? "top" : "bottom");

  // Held off the leading rail's observed position. Departure proves that it
  // left, not that it will reach its unacknowledged target; using that target
  // here would let a follower cross a lead whose write was lost or which
  // stalled immediately afterwards. Any temporary truncation is completed by
  // the bounded residual pass once both rails have settled.
  if (!this->command_(this->trail_, this->trail_target_, lead_position)) {
    this->abandon_("the second rail did not accept its command");
    return;
  }
  this->has_trail_ = false;
  this->wait_ = Wait::NONE;
  this->phase_ = Phase::TRAILING;
  this->phase_since_ = millis();
}

MotionblindsBLETdbu::ResidualResult MotionblindsBLETdbu::complete_residual_() {
  // The safety net behind the clamping rule above. A target can still come out
  // short of what was asked -- the leading rail was itself clamped, or stopped,
  // or its own range ran out -- and nothing else would ever issue the rest of
  // it, leaving the blind at the wrong length and the wrong centre for good.
  if (!this->fresh_(Rail::TOP) || !this->fresh_(Rail::BOTTOM))
    return ResidualResult::NONE;  // never move on a remembered position

  for (const Rail rail : {Rail::TOP, Rail::BOTTOM}) {
    RailCommand &record = this->commanded_[rail == Rail::TOP ? 0 : 1];
    if (!record.active || record.residual_attempted)
      continue;
    const float quantum = rail_quantum(this->motor_(rail)->rail_range());
    const float shortfall = std::fabs(record.intended - record.achieved);
    if (quantum <= 0.0f || shortfall <= quantum)
      continue;

    // Recomputed against where the other rail has actually ended up, so this
    // only ever asks for what the geometry now genuinely allows.
    const Rail other = rail == Rail::TOP ? Rail::BOTTOM : Rail::TOP;
    const uint8_t raw = this->geometry_.raw_target(rail, record.intended, this->position_(other));
    const float achievable = this->motor_(rail)->rail_range().to_window(static_cast<float>(raw));
    if (std::fabs(achievable - record.achieved) <= quantum)
      continue;  // no closer than what was already commanded

    record.residual_attempted = true;
    if (!this->motor_(rail)->request_position(achievable, this->current_request_at_)) {
      ESP_LOGE(TAG, "%s rail still needed %.1f%% but did not accept the residual command",
               rail == Rail::TOP ? "Top" : "Bottom", achievable);
      return ResidualResult::FAILED;
    }
    ESP_LOGI(TAG, "%s rail was cut short at %.1f%% of the %.1f%% asked for; finishing at %.1f%%",
             rail == Rail::TOP ? "Top" : "Bottom", record.achieved, record.intended, achievable);
    record.achieved = achievable;
    return ResidualResult::STARTED;
  }
  return ResidualResult::NONE;
}

void MotionblindsBLETdbu::finish_() {
  this->phase_ = Phase::IDLE;
  this->has_trail_ = false;
  this->wait_ = Wait::NONE;
  this->lead_reference_ = NAN;
  this->lead_reference_fresh_ = false;
  this->lead_command_at_ = 0;
  this->lead_committed_ = 0.0f;
  this->commanded_[0] = RailCommand{};
  this->commanded_[1] = RailCommand{};
  this->operation_since_ = 0;
  this->current_request_at_ = 0;
  this->prepared_ = false;
  this->prepared_since_ = 0;
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
    if (this->pending_.active) {
      const uint32_t elapsed = millis() - this->pending_.submitted_at;
      if (rail_request_ready(this->pending_.combined, this->pending_.has_top, this->pending_.has_bottom, elapsed,
                             RAIL_PAIR_WINDOW_MS))
        this->begin_();
    } else if (this->prepared_ && millis() - this->prepared_since_ > this->prepare_timeout_) {
      ESP_LOGI(TAG, "Prepared connection window expired; releasing both rails");
      this->prepared_ = false;
      this->prepared_since_ = 0;
      this->release_leases_();
      this->update_callback_.call();
    }
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
        this->abandon_(this->wait_ == Wait::DEPARTURE ? "the first rail never started moving"
                                                     : "the first rail never made room");
      break;

    case Phase::TRAILING:
      // Wait on the motors, not on is_moving(): that reports this phase too,
      // so the condition could never come true and every move would run until
      // the lease expired.
      if (!this->motors_busy_()) {
        // One last chance to deliver a target the geometry had to cut short
        // while the other rail was still in the way. Bounded to a single extra
        // command, and it keeps the phase open until that command resolves.
        const ResidualResult residual = this->complete_residual_();
        if (residual == ResidualResult::STARTED) {
          this->phase_since_ = now;
          break;
        }
        if (residual == ResidualResult::FAILED) {
          this->abandon_("a rail did not accept its residual position command");
          break;
        }
        this->finish_();
      }
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
