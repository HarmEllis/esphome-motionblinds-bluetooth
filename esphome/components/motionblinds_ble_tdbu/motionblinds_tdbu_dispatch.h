#pragma once

#include <cmath>
#include <cstdint>

#include "motionblinds_tdbu_geometry.h"

namespace esphome::motionblinds_ble_tdbu {

using esphome::motionblinds_ble::RailRange;

/* When the two rails of one blind may be set off, and what counts as proof
 * that the first one has left.
 *
 * The coordinator used to decide this from the observed gap alone: below
 * `start_gap` the second rail waited, above it both started together. That rule
 * is safe but it is blind to *which way* the rails are going, and the direction
 * is what actually decides whether waiting buys anything:
 *
 *   both rails AWAY      the gap can only grow, from the first millimetre of
 *                        travel onwards. Waiting protects nothing, and this is
 *                        the most common command there is — opening or closing
 *                        a blind whose rails are stacked together.
 *   one AWAY, one TOWARD a pure translation: both rails move the same physical
 *                        direction by the same amount, so **the gap never
 *                        changes**. `gap >= start_gap` therefore can never
 *                        become true if it was not true at dispatch, and the
 *                        old rule silently degraded to "the second rail waits
 *                        out the first one's entire travel". What is actually
 *                        needed is proof that the gap-opening rail has left, so
 *                        the trailing rail is following it rather than driving
 *                        into a stationary obstacle.
 *   both rails TOWARD    the rails converge. Nothing about starting early is
 *                        safe here, so this keeps the conservative rule.
 *
 * Kept free of ESPHome, Bluetooth and motor state so the whole policy — start
 * rule and departure proof alike — can be swept exhaustively on the host.
 */

/// What has to be true before the trailing rail's command may be written.
enum class StartRule : uint8_t {
  TOGETHER,         ///< both commands go out in the same pass
  AFTER_DEPARTURE,  ///< the leading rail must be observed to have moved away first
  AFTER_CLEARANCE,  ///< the conservative rule: `start_gap` observed, or the lead settled
};

struct DispatchPlan {
  Rail lead{Rail::TOP};
  Rail trail{Rail::BOTTOM};
  bool has_trail{false};        ///< whether a second rail moves at all
  bool moves{false};            ///< whether anything moves at all
  bool same_direction{false};   ///< a translation: both rails travel the same physical way
  bool both_away{false};         ///< both intermediate movements can only increase the gap
  StartRule rule{StartRule::TOGETHER};
};

struct DispatchRequest {
  Direction top{Direction::STATIONARY};
  Direction bottom{Direction::STATIONARY};
  /// Observed distance between the rails right now.
  float gap{0.0f};
  float start_gap{0.0f};
  /// The escape hatch. False restores the pre-existing gap-only rule exactly.
  bool direction_aware{true};
  /// Both motors have reported a speed setting and the two differ. A trailing
  /// rail that travels faster than the one it is following closes the gap for
  /// the whole move, and departure proof says nothing about relative speed.
  bool speeds_known_different{false};
};

inline DispatchPlan plan_dispatch(const DispatchRequest &request) {
  DispatchPlan plan;

  const bool top_moves = request.top != Direction::STATIONARY;
  const bool bottom_moves = request.bottom != Direction::STATIONARY;
  plan.moves = top_moves || bottom_moves;
  if (!plan.moves)
    return plan;

  // One rail only: there is no second command to hold back.
  if (!bottom_moves) {
    plan.lead = Rail::TOP;
    plan.trail = Rail::BOTTOM;
    return plan;
  }
  if (!top_moves) {
    plan.lead = Rail::BOTTOM;
    plan.trail = Rail::TOP;
    return plan;
  }

  // Both rails move. The one whose travel opens the gap leads; if neither does
  // (both closing in) the top rail leads by convention and the wait is what
  // keeps them apart.
  const bool top_leads = request.top == Direction::AWAY || request.bottom != Direction::AWAY;
  plan.lead = top_leads ? Rail::TOP : Rail::BOTTOM;
  plan.trail = top_leads ? Rail::BOTTOM : Rail::TOP;
  plan.has_trail = true;

  const bool both_away = request.top == Direction::AWAY && request.bottom == Direction::AWAY;
  plan.both_away = both_away;
  plan.same_direction = (request.top == Direction::AWAY) != (request.bottom == Direction::AWAY);

  const bool roomy = request.gap >= request.start_gap;

  if (!request.direction_aware) {
    plan.rule = roomy ? StartRule::TOGETHER : StartRule::AFTER_CLEARANCE;
    return plan;
  }

  if (both_away) {
    // Every intermediate state of both moves increases the gap, and overshoot
    // can only increase it further. `start_gap` has nothing to say here.
    plan.rule = StartRule::TOGETHER;
    return plan;
  }

  if (roomy) {
    plan.rule = StartRule::TOGETHER;
    return plan;
  }

  if (plan.same_direction && !request.speeds_known_different) {
    plan.rule = StartRule::AFTER_DEPARTURE;
    return plan;
  }

  plan.rule = StartRule::AFTER_CLEARANCE;
  return plan;
}

/// One raw step of a rail's own reported range, in window units. The motors
/// report whole percents of their own travel, so this is the smallest movement
/// that can possibly be observed — 1.0 on a full rail, 0.4 on one calibrated
/// over 40% of the window.
inline float rail_quantum(const RailRange &range) {
  const float span = range.window_max - range.window_min;
  return span > 0.0f ? span / 100.0f : 0.0f;
}

/// Everything the departure decision is allowed to look at. Note what is
/// absent: the motor's locally-set "moving" flag. It is set at write time and
/// says only that this node wrote something, which is exactly the claim that
/// must not be believed.
struct DepartureEvidence {
  Rail lead{Rail::TOP};
  /// The leading rail's current position was observed on the current link.
  bool lead_fresh{false};
  /// The baseline was an observation too, not a remembered position. A
  /// remembered position that turns out to be wrong looks exactly like movement.
  bool baseline_fresh{false};
  /// The leading rail's position write has actually gone out, rather than
  /// merely having been accepted by the coordinator. Without this, a remote
  /// nudging the lead while its command is still queued reads as our command
  /// taking effect.
  bool write_sent{false};
  float baseline{0.0f};
  float now{0.0f};
  /// One raw quantum of the leading rail's own range.
  float quantum{0.0f};
  float gap{0.0f};
  float effective_gap{0.0f};
};

inline bool departure_proven(const DepartureEvidence &evidence) {
  if (!evidence.lead_fresh || !evidence.baseline_fresh || !evidence.write_sent)
    return false;
  if (evidence.quantum <= 0.0f)
    return false;

  // Movement the wrong way is never proof, however large.
  const float travelled =
      evidence.lead == Rail::TOP ? evidence.baseline - evidence.now : evidence.now - evidence.baseline;
  if (travelled + 0.001f < evidence.quantum)
    return false;

  // The floor that survives a lead which departs and then stalls.
  return evidence.gap + 0.001f >= evidence.effective_gap;
}

}  // namespace esphome::motionblinds_ble_tdbu
