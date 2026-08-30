// Host tests for the two bounded recovery paths behind an absolute position
// write: the post-travel one that may condemn a move, and the early no-start
// one that never may.

#include <cstdio>

#include "motionblinds_ble_recovery.h"

using namespace esphome::motionblinds_ble;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    failures++;
    std::printf("FAIL: %s\n", what);
  }
}

StartCheckInputs armed() {
  StartCheckInputs in;
  in.settled_position = true;
  in.origin_known = true;
  in.movement_observed = false;
  in.post_travel = false;
  in.verify_start_after = DEFAULT_VERIFY_START_AFTER_MS;
  in.since_write = 0;
  in.since_query = 0;
  in.queries = 0;
  return in;
}

void test_post_travel_recovery() {
  check(position_recovery_action(20, 20, 1) == PositionRecoveryAction::REACHED,
        "a confirmed target is complete");
  check(position_recovery_action(90, 20, 1) == PositionRecoveryAction::RETRY,
        "a mismatched status retries the first unacknowledged write");
  check(position_recovery_action(90, 20, 2) == PositionRecoveryAction::EXHAUSTED,
        "the retry is bounded after the second delivery");
}

void test_start_check_timing() {
  StartCheckInputs in = armed();

  in.since_write = DEFAULT_VERIFY_START_AFTER_MS - 1;
  check(start_check_action(in) == StartCheckAction::WAIT, "nothing is asked before the delay");

  in.since_write = DEFAULT_VERIFY_START_AFTER_MS;
  check(start_check_action(in) == StartCheckAction::QUERY, "the question is asked once the delay is up");

  // A motor that has moved is doing what it was told. This is the "never retry
  // a motor that has moved" rule, asserted where it is decided.
  in.movement_observed = true;
  check(start_check_action(in) == StartCheckAction::WAIT, "an observed movement stops the check for good");

  in = armed();
  in.since_write = 60000;
  in.origin_known = false;
  check(start_check_action(in) == StartCheckAction::WAIT, "without a starting position there is nothing to confirm");

  in = armed();
  in.since_write = 60000;
  in.settled_position = false;
  check(start_check_action(in) == StartCheckAction::WAIT, "only an absolute position awaiting settlement is checked");

  in = armed();
  in.since_write = 60000;
  in.verify_start_after = 0;
  check(start_check_action(in) == StartCheckAction::WAIT, "zero disables the check entirely");

  // Once the travel budget has run out its own recheck owns the decision, and
  // that is the only path allowed to fail a move.
  in = armed();
  in.since_write = 60000;
  in.post_travel = true;
  check(start_check_action(in) == StartCheckAction::WAIT, "the post-travel recheck takes the decision over");
}

void test_start_check_is_bounded_and_spaced() {
  check(MAX_START_QUERIES == REQUIRED_ORIGIN_CONFIRMATIONS + 1,
        "the query budget includes one lost round trip beyond both confirmations");
  StartCheckInputs in = armed();
  in.since_write = 60000;
  in.queries = 1;

  in.since_query = START_QUERY_RETRY_MS - 1;
  check(start_check_action(in) == StartCheckAction::WAIT, "a second question waits out the retry spacing");
  in.since_query = START_QUERY_RETRY_MS;
  check(start_check_action(in) == StartCheckAction::QUERY, "and is asked once that has passed");

  in.queries = MAX_START_QUERIES;
  in.since_query = 600000;
  check(start_check_action(in) == StartCheckAction::IDLE, "the questions are bounded");

  // The important half of that: beyond the bound there are no more queries.
  for (uint8_t queries = 0; queries <= MAX_START_QUERIES + 3; queries++) {
    in.queries = queries;
    const StartCheckAction action = start_check_action(in);
    if (queries >= MAX_START_QUERIES)
      check(action == StartCheckAction::IDLE, "the early check stays idle beyond its query bound");
  }
}

bool reaches_early_retry_with_lost_round_trip(uint8_t lost_query) {
  StartCheckInputs in = armed();
  in.since_write = DEFAULT_VERIFY_START_AFTER_MS;
  in.since_query = START_QUERY_RETRY_MS;
  uint8_t confirmations = 0;

  for (uint8_t query = 1; query <= MAX_START_QUERIES; query++) {
    in.queries = query - 1;
    if (start_check_action(in) != StartCheckAction::QUERY)
      return false;
    if (query == lost_query)
      continue;  // either the unacknowledged query or its answer disappeared

    const StartAnswerAction answer = start_answer_action(30, 30, 70, 1, confirmations);
    if (answer == StartAnswerAction::RETRY)
      return true;
    if (answer != StartAnswerAction::CONFIRM)
      return false;
    confirmations++;
  }
  return false;
}

void test_start_check_tolerates_one_lost_round_trip() {
  check(reaches_early_retry_with_lost_round_trip(0), "two matching answers retry when no round trip is lost");
  for (uint8_t lost = 1; lost <= MAX_START_QUERIES; lost++)
    check(reaches_early_retry_with_lost_round_trip(lost),
          "one lost start-verification round trip still reaches the early retry");
}

void test_start_answer() {
  // Only two explicit answers at the exact starting position justify sending
  // the command again. The pause between them gives a slow starter another
  // chance to report real movement.
  check(start_answer_action(30, 30, 70, 1, 0) == StartAnswerAction::CONFIRM,
        "one unchanged status asks for confirmation rather than re-sending");
  check(start_answer_action(30, 30, 70, 1, 1) == StartAnswerAction::RETRY,
        "two unchanged statuses re-send the write");
  check(start_answer_action(31, 30, 70, 1, 1) == StartAnswerAction::IGNORE,
        "one raw step away from the origin is movement, so nothing is re-sent");
  check(start_answer_action(70, 30, 70, 1, 1) == StartAnswerAction::IGNORE,
        "a rail that has arrived is never re-sent");
  check(start_answer_action(30, 30, 70, MAX_POSITION_DELIVERY_ATTEMPTS, 1) == StartAnswerAction::DONE,
        "an exhausted delivery budget stops the early path without failing the move");

  // A wrong origin — a restored position that no longer holds — can only cost a
  // missed retry, never cause a false one.
  check(start_answer_action(55, 30, 70, 1, 1) == StartAnswerAction::IGNORE,
        "a wrong remembered origin only ever misses a retry");
}

void test_verify_start_after_clamp() {
  check(clamp_verify_start_after(0) == 0, "zero stays zero, which is how the check is disabled");
  check(clamp_verify_start_after(1000) == MIN_VERIFY_START_AFTER_MS, "a too-short delay is raised to the floor");
  check(clamp_verify_start_after(MIN_VERIFY_START_AFTER_MS) == MIN_VERIFY_START_AFTER_MS, "the floor itself stands");
  check(clamp_verify_start_after(DEFAULT_VERIFY_START_AFTER_MS) == DEFAULT_VERIFY_START_AFTER_MS,
        "the default is left alone");
  check(DEFAULT_VERIFY_START_AFTER_MS >= MIN_VERIFY_START_AFTER_MS, "the default respects its own floor");
}

}  // namespace

int main() {
  test_post_travel_recovery();
  test_start_check_timing();
  test_start_check_is_bounded_and_spaced();
  test_start_check_tolerates_one_lost_round_trip();
  test_start_answer();
  test_verify_start_after_clamp();

  if (failures == 0)
    std::printf("position recovery: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
