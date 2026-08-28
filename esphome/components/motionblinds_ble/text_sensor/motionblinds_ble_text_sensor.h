#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

/// Reports where a motor is in its connection lifecycle, including the error
/// state a failed operation leaves behind. Waiting on this is how an
/// automation can tell a real failure from a slow motor.
class MotionblindsBLETextSensor : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }
  void set_status_sensor(text_sensor::TextSensor *sensor) { this->status_ = sensor; }

 protected:
  void update_();

  MotionblindsBLEMotor *motor_{nullptr};
  text_sensor::TextSensor *status_{nullptr};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
