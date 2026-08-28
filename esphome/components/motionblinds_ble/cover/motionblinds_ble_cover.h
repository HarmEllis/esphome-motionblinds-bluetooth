#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/cover/cover.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

/// A single motor presented as one cover.
///
/// Closed means the rail covers as much of the window as it can, which is
/// window_max, and open is window_min. Going through the window transform
/// rather than straight to the raw protocol position means `invert` and a
/// partial calibration behave the same here as they do for a top-down
/// bottom-up blind.
class MotionblindsBLECover : public cover::Cover, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  cover::CoverTraits get_traits() override;

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }

 protected:
  void control(const cover::CoverCall &call) override;
  void update_state_();

  MotionblindsBLEMotor *motor_{nullptr};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
