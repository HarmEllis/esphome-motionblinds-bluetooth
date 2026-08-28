#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

enum class ButtonAction : uint8_t {
  STATUS_QUERY,
  FAVORITE,
  CONNECT,
  DISCONNECT,
};

class MotionblindsBLEButton : public button::Button, public Component {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }
  void set_action(ButtonAction action) { this->action_ = action; }

 protected:
  void press_action() override;

  MotionblindsBLEMotor *motor_{nullptr};
  ButtonAction action_{ButtonAction::STATUS_QUERY};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
