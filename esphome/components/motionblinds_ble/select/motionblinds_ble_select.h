#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

/// Motor speed. Writing it is not verifiable: the motor never reports back
/// that a speed command was applied, so the selection is optimistic until the
/// next status frame happens to confirm it.
class MotionblindsBLESpeedSelect : public select::Select, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }

 protected:
  void control(const std::string &value) override;
  void update_();

  MotionblindsBLEMotor *motor_{nullptr};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
