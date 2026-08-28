#pragma once

#include <cmath>
#include <cstdint>

#include "esphome/components/motionblinds_ble/motionblinds_rail.h"

namespace esphome::motionblinds_ble_tdbu {

using esphome::motionblinds_ble::RailRange;

/* Geometry of a top-down bottom-up blind.
 *
 * Two motors, one per rail, each unaware of the other. Everything here works
 * in *window coordinates*: 0 is the top of the window, 100 the bottom, and the
 * invariant that must hold at all times is
 *
 *     top + min_gap <= bottom
 *
 * The two supported fabrics are geometrically the same problem. In both cases
 * the rails delimit a segment of length L = bottom - top; only its meaning and
 * therefore the direction of "open" differ:
 *
 *   between_rails  the fabric hangs between the rails, so L is the covered
 *                  part of the window. Long L = closed.
 *   outside_in     two sheets close in from the top and bottom, so L is the
 *                  see-through gap between them. Long L = open.
 *
 * Recognising that both are "place a segment of length L centred near c" is
 * what lets one set of feasibility and rounding rules serve both, instead of
 * two nearly-identical implementations that drift apart.
 *
 * This header is deliberately free of ESPHome, Bluetooth and floating-point
 * surprises so the collision rules can be exhaustively tested on the host.
 */

enum class Fabric : uint8_t {
  BETWEEN_RAILS,
  OUTSIDE_IN,
};

enum class Rail : uint8_t {
  TOP,
  BOTTOM,
};

/// Which way a rail's target moves it relative to the other rail.
enum class Direction : uint8_t {
  AWAY,        ///< the gap between the rails grows
  STATIONARY,  ///< no meaningful change
  TOWARD,      ///< the gap between the rails shrinks
};

/// Where a segment of a given length ended up after being fitted into what the
/// two rails can actually reach.
struct Placement {
  float top{0.0f};
  float bottom{0.0f};
  float length{0.0f};  ///< the length actually achieved, which may be shorter than requested
  bool feasible{false};
};

class Geometry {
 public:
  Geometry() = default;
  Geometry(Fabric fabric, float min_gap, float safety_margin, RailRange top, RailRange bottom)
      : fabric_(fabric), min_gap_(min_gap), safety_margin_(safety_margin), top_(top), bottom_(bottom) {}

  Fabric fabric() const { return this->fabric_; }
  const RailRange &top_range() const { return this->top_; }
  const RailRange &bottom_range() const { return this->bottom_; }

  /// The clearance actually used when computing targets: the physical minimum
  /// plus a margin for integer feedback and motor overshoot.
  float effective_gap() const { return this->min_gap_ + this->safety_margin_; }
  float min_gap() const { return this->min_gap_; }

  bool valid() const {
    return this->top_.valid() && this->bottom_.valid() && this->min_gap_ >= 0.0f && this->min_gap_ < 100.0f &&
           this->safety_margin_ >= 0.0f && this->max_length() >= this->min_length();
  }

  /// Shortest segment the rails may form. Two rails whose travel ranges do
  /// not overlap cannot come closer than the distance between those ranges,
  /// which can exceed the requested clearance.
  float min_length() const {
    const float forced = this->bottom_.window_min - this->top_.window_max;
    return forced > this->effective_gap() ? forced : this->effective_gap();
  }

  /// Longest segment the rails can form given their travel ranges. With
  /// partially calibrated rails this is less than 100, and the entity scales
  /// are normalised against it so that 0% and 100% stay reachable.
  float max_length() const {
    const float span = this->bottom_.window_max - this->top_.window_min;
    return span > 100.0f ? 100.0f : span;
  }

  /// Fit a segment of `length` into both rails' travel, keeping its centre as
  /// close to `preferred_centre` as the ranges allow.
  ///
  /// Shifting the centre rather than refusing keeps the extremes reachable: a
  /// user asking for fully open gets fully open even when the current centre
  /// could not accommodate it.
  Placement place_segment(float length, float preferred_centre) const {
    Placement result;

    // top = c - L/2 must sit in the top rail's range and bottom = c + L/2 in
    // the bottom rail's. Both are intervals in c, and requiring their
    // intersection to be non-empty reduces to a closed-form bound on L, so no
    // search or recursion is needed.
    const float requested = length;
    length = clamp(length, this->min_length(), this->max_length());
    result.feasible = std::fabs(length - requested) < 0.001f;

    const float half = length / 2.0f;
    const float lo = std::fmax(this->top_.window_min + half, this->bottom_.window_min - half);
    const float hi = std::fmin(this->top_.window_max + half, this->bottom_.window_max - half);
    if (lo > hi)
      return result;  // min_length()/max_length() should prevent this

    const float centre = clamp(preferred_centre, lo, hi);
    result.top = centre - half;
    result.bottom = centre + half;
    result.length = length;
    return result;
  }

  /// Openness of the blind as a whole, 0 closed to 1 open.
  float length_to_openness(float length) const {
    const float span = this->max_length() - this->min_length();
    if (span <= 0.0f)
      return 0.0f;
    const float fraction = clamp((length - this->min_length()) / span, 0.0f, 1.0f);
    // A long segment means a covered window for between_rails and an open one
    // for outside_in; that single flip is the whole difference between them.
    return this->fabric_ == Fabric::OUTSIDE_IN ? fraction : 1.0f - fraction;
  }

  float openness_to_length(float openness) const {
    openness = clamp(openness, 0.0f, 1.0f);
    const float fraction = this->fabric_ == Fabric::OUTSIDE_IN ? openness : 1.0f - openness;
    return this->min_length() + fraction * (this->max_length() - this->min_length());
  }

  /// How far a single rail may travel while leaving the other one alone.
  void rail_travel(Rail rail, float other_position, float &lo, float &hi) const {
    if (rail == Rail::TOP) {
      lo = this->top_.window_min;
      hi = std::fmin(this->top_.window_max, other_position - this->effective_gap());
    } else {
      lo = std::fmax(this->bottom_.window_min, other_position + this->effective_gap());
      hi = this->bottom_.window_max;
    }
  }

  /// How high a rail is sitting, as a Home Assistant cover position (0-1):
  /// 1.0 raised to the top of its own travel, 0.0 lowered to the bottom of it.
  ///
  /// The same for both rails, so Home Assistant's own cover arrows move a rail
  /// in the direction they point. That costs the usual "100 is open" reading
  /// for the top rail — raised is where it covers the *most*, because the
  /// fabric hangs from it — but a control that moves the blind the other way
  /// from the arrow you pressed is worse than a label that reads oddly.
  ///
  /// Deliberately absolute: the scale is the rail's own travel, not the travel
  /// left over by the other rail. Scaling against the other rail made a rail's
  /// reported position change whenever the *other* one moved, and made a
  /// requested 50% land somewhere different depending on where the other rail
  /// happened to be.
  float rail_position(Rail rail, float window) const {
    const RailRange &range = rail == Rail::TOP ? this->top_ : this->bottom_;
    const float span = range.window_max - range.window_min;
    if (span <= 0.0f)
      return 0.0f;
    const float travelled = clamp((window - range.window_min) / span, 0.0f, 1.0f);
    return 1.0f - travelled;
  }

  /// Inverse of rail_position. The result is a request, not a promise: it still
  /// has to go through clamp_target() before it can be commanded.
  float rail_window_target(Rail rail, float position) const {
    const RailRange &range = rail == Rail::TOP ? this->top_ : this->bottom_;
    const float span = range.window_max - range.window_min;
    const float travelled = clamp(1.0f - position, 0.0f, 1.0f);
    return range.window_min + travelled * span;
  }

  /// Last line of defence for absolute moves and for races where the other
  /// rail moved in between. Clamping rather than refusing keeps automations
  /// predictable.
  float clamp_target(Rail rail, float target, float other_position) const {
    float lo, hi;
    this->rail_travel(rail, other_position, lo, hi);
    if (hi < lo)  // the rails are already too close; do not make it worse
      return rail == Rail::TOP ? std::fmin(target, hi) : std::fmax(target, lo);
    return clamp(target, lo, hi);
  }

  /// Classify a move by what it does to the distance between the rails, which
  /// is what decides the order the two rails may be commanded in.
  static Direction classify(Rail rail, float current, float target) {
    constexpr float EPSILON = 0.5f;  // below the motors' own reporting quantum
    const float delta = target - current;
    if (std::fabs(delta) < EPSILON)
      return Direction::STATIONARY;
    if (rail == Rail::TOP)
      return delta < 0.0f ? Direction::AWAY : Direction::TOWARD;
    return delta > 0.0f ? Direction::AWAY : Direction::TOWARD;
  }

  /// Convert a window target into the integer the motor is actually given,
  /// rounding so that quantisation can only ever increase the clearance.
  ///
  /// Rounding outward has to happen after the inversion, because inverting a
  /// rail reverses which raw direction corresponds to "away".
  uint8_t raw_target(Rail rail, float window_target, float other_position) const {
    const RailRange &range = rail == Rail::TOP ? this->top_ : this->bottom_;
    const float clamped = this->clamp_target(rail, window_target, other_position);
    const float raw = range.to_raw(clamped);

    // Try the two neighbouring integers, preferring the one closest to the
    // request that still keeps the required clearance.
    const int lower = static_cast<int>(std::floor(raw));
    const int upper = static_cast<int>(std::ceil(raw));
    const int candidates[2] = {lower, upper};

    int best = -1;
    float best_error = 0.0f;
    for (int candidate : candidates) {
      if (candidate < 0 || candidate > 100)
        continue;
      const float achieved = range.to_window(static_cast<float>(candidate));
      const float gap = rail == Rail::TOP ? other_position - achieved : achieved - other_position;
      if (gap + 0.001f < this->min_gap_)
        continue;
      const float error = std::fabs(achieved - clamped);
      if (best < 0 || error < best_error) {
        best = candidate;
        best_error = error;
      }
    }

    if (best >= 0)
      return static_cast<uint8_t>(best);

    // Neither neighbour clears the invariant, so retreat to the furthest the
    // rail can be from the other one within its own range.
    const float safest = rail == Rail::TOP ? range.window_min : range.window_max;
    const float raw_safest = range.to_raw(safest);
    return static_cast<uint8_t>(raw_safest + 0.5f);
  }

  static float clamp(float value, float lo, float hi) { return value < lo ? lo : (value > hi ? hi : value); }

 private:
  Fabric fabric_{Fabric::BETWEEN_RAILS};
  float min_gap_{5.0f};
  float safety_margin_{2.0f};
  RailRange top_{};
  RailRange bottom_{};
};

}  // namespace esphome::motionblinds_ble_tdbu
