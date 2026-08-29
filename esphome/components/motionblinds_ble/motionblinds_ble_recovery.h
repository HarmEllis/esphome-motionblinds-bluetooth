#pragma once

#include <cstdint>

namespace esphome::motionblinds_ble {

/// Position writes have no protocol acknowledgement. A status response at a
/// different position is therefore evidence that the absolute command needs
/// to be delivered again, not that asking the same question three times will
/// make the motor move.
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

}  // namespace esphome::motionblinds_ble
