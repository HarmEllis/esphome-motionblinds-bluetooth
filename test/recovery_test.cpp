// Host test for bounded recovery of unacknowledged absolute position writes.

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

}  // namespace

int main() {
  check(position_recovery_action(20, 20, 1) == PositionRecoveryAction::REACHED,
        "a confirmed target is complete");
  check(position_recovery_action(90, 20, 1) == PositionRecoveryAction::RETRY,
        "a mismatched status retries the first unacknowledged write");
  check(position_recovery_action(90, 20, 2) == PositionRecoveryAction::EXHAUSTED,
        "the retry is bounded after the second delivery");

  if (failures == 0)
    std::printf("position recovery: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
