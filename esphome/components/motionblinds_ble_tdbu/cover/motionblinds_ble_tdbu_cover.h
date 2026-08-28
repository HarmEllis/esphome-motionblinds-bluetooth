#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/cover/cover.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble_tdbu.h"

namespace esphome::motionblinds_ble_tdbu {

/// Which part of the blind a cover entity drives.
enum class CoverRail : uint8_t {
  TOP,
  BOTTOM,
  COMBINED,
};

/// One cover entity for a top-down bottom-up blind.
///
/// Position always means openness, for every rail and both fabrics: 0 is
/// closed, 1 is open, and more fabric across the window is more closed. The
/// Home Assistant integration inherits the gateway's opposite convention for
/// the top rail, where a raised rail counts as "open" while covering the most
/// window; that is not carried over here.
///
/// Sliding the fabric block without changing how much of it shows is a
/// different physical quantity, and lives on a separate number entity rather
/// than being folded into this one.
class MotionblindsBLETdbuCover : public cover::Cover, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  cover::CoverTraits get_traits() override;

  void set_tdbu(MotionblindsBLETdbu *tdbu) { this->tdbu_ = tdbu; }
  void set_rail(CoverRail rail) { this->rail_ = rail; }

 protected:
  void control(const cover::CoverCall &call) override;
  void update_state_();

  MotionblindsBLETdbu *tdbu_{nullptr};
  CoverRail rail_{CoverRail::COMBINED};
};

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
