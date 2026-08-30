#pragma once

#include <cstdint>

namespace esphome::motionblinds_ble {

/// Shared total delivery budget for an absolute position. Both the early
/// no-start check and the later post-travel recovery consume this same budget:
/// once either path re-sends, the other may never add a third position write.
static constexpr uint8_t MAX_POSITION_DELIVERY_ATTEMPTS = 2;

enum class PositionRecoveryAction : uint8_t {
  REACHED,
  RETRY,
  EXHAUSTED,
};

constexpr PositionRecoveryAction position_recovery_action(uint8_t reported, uint8_t target,
                                                           uint8_t delivery_attempts) {
  if (reported == target)
    return PositionRecoveryAction::REACHED;
  if (delivery_attempts < MAX_POSITION_DELIVERY_ATTEMPTS)
    return PositionRecoveryAction::RETRY;
  return PositionRecoveryAction::EXHAUSTED;
}

/* Early no-start verification.
 *
 * The travel budget answers "did this move finish", and it deliberately answers
 * slowly: eight seconds plus 700 ms per percent, then up to three status
 * rechecks, then at most one re-delivery. A write that never reached the motor
 * at all therefore costs half a minute before anything is tried again — and a
 * lost write is completely silent, because these commands are written without a
 * response.
 *
 * A rail that has not moved *at all* a few seconds after the write is a much
 * earlier and much narrower signal. It is still not proof: the motor may simply
 * be slow off the mark, and a re-send landing on a moving motor is the exact
 * condition that makes these blinds do nothing at all. So this path never acts
 * on silence. It asks — bounded, and spaced — and re-sends only when an explicit
 * STATUS frame reports the position the rail was standing on when the command
 * went out.
 *
 * Two consequences of that, both deliberate:
 *
 *  * it cannot fail a move. An exhausted early check is silence, not
 *    condemnation; only the post-travel path may declare a move failed.
 *  * a wrong starting position can only cause a *missed* early retry, never a
 *    false one, because the answer can then only differ from it. That is what
 *    makes it safe to run without requiring a freshly observed position.
 */

/// Least sensible delay before asking. Below the interval two commands to one
/// motor must be spaced by, a healthy motor can still be inside its first
/// reported percent and the re-send would land on a rail that is moving.
static constexpr uint32_t MIN_VERIFY_START_AFTER_MS = 3000;
/// Default delay. Field logs record a rail taking four seconds to move a few
/// percent, so three is cutting it close enough to be worth a second.
static constexpr uint32_t DEFAULT_VERIFY_START_AFTER_MS = 5000;
/// Require two explicit unchanged answers before re-delivering. One answer at
/// five seconds can still catch a healthy motor immediately before its first
/// reported position change; the second leaves time for that movement to show.
static constexpr uint8_t REQUIRED_ORIGIN_CONFIRMATIONS = 2;
/// One query per required confirmation, plus one spare because the query write
/// or its answer can itself be lost on the same lossy link being diagnosed.
static constexpr uint8_t MAX_START_QUERIES = REQUIRED_ORIGIN_CONFIRMATIONS + 1;
static_assert(MAX_START_QUERIES > REQUIRED_ORIGIN_CONFIRMATIONS,
              "start verification needs one loss-tolerant query beyond its confirmations");
static constexpr uint32_t START_QUERY_RETRY_MS = 2000;

enum class StartCheckAction : uint8_t {
  WAIT,   ///< too early, or nothing to check; keep waiting on the travel budget
  QUERY,  ///< ask the motor where it is, without touching the travel budget
  IDLE,   ///< the bounded questions are used up; never a failure
};

struct StartCheckInputs {
  /// The in-flight command is an absolute position awaiting SETTLED
  /// verification. Anything else — a downgraded no-op, a stop, a query — has no
  /// starting position to confirm.
  bool settled_position{false};
  /// A starting position was known when the write went out.
  bool origin_known{false};
  /// A frame has since reported a position other than that origin.
  bool movement_observed{false};
  /// The post-travel recheck has taken over; it owns the decision from there.
  bool post_travel{false};
  /// 0 disables the check entirely.
  uint32_t verify_start_after{0};
  uint32_t since_write{0};
  uint32_t since_query{0};
  uint8_t queries{0};
};

constexpr StartCheckAction start_check_action(const StartCheckInputs &in) {
  if (!in.settled_position || !in.origin_known || in.verify_start_after == 0)
    return StartCheckAction::WAIT;
  // A motor that has moved is doing what it was told; the travel budget owns it.
  if (in.movement_observed || in.post_travel)
    return StartCheckAction::WAIT;
  if (in.queries == 0)
    return in.since_write >= in.verify_start_after ? StartCheckAction::QUERY : StartCheckAction::WAIT;
  if (in.queries >= MAX_START_QUERIES)
    return StartCheckAction::IDLE;
  return in.since_query >= START_QUERY_RETRY_MS ? StartCheckAction::QUERY : StartCheckAction::WAIT;
}

enum class StartAnswerAction : uint8_t {
  IGNORE,  ///< the rail is not where it started, or is already there; do nothing
  CONFIRM, ///< one unchanged answer is not enough; ask again after the spacing
  RETRY,   ///< it never left; deliver the absolute position again
  DONE,    ///< it never left but the bounded deliveries are used up; still not a failure
};

constexpr StartAnswerAction start_answer_action(uint8_t reported, uint8_t origin, uint8_t target,
                                                uint8_t delivery_attempts, uint8_t origin_confirmations) {
  if (reported == target)
    return StartAnswerAction::IGNORE;
  if (reported != origin)
    return StartAnswerAction::IGNORE;
  if (delivery_attempts < MAX_POSITION_DELIVERY_ATTEMPTS)
    return origin_confirmations + 1 >= REQUIRED_ORIGIN_CONFIRMATIONS ? StartAnswerAction::RETRY
                                                                     : StartAnswerAction::CONFIRM;
  return StartAnswerAction::DONE;
}

/// Second line of defence behind the schema, which rejects anything shorter
/// outright. 0 stays 0: that is the documented way to switch the check off.
constexpr uint32_t clamp_verify_start_after(uint32_t ms) {
  if (ms == 0)
    return 0;
  return ms < MIN_VERIFY_START_AFTER_MS ? MIN_VERIFY_START_AFTER_MS : ms;
}

}  // namespace esphome::motionblinds_ble
