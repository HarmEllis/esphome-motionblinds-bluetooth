#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

class MotionblindsBLEBinarySensor : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }
  void set_fresh_sensor(binary_sensor::BinarySensor *sensor) { this->fresh_ = sensor; }
  void set_calibration_sensor(binary_sensor::BinarySensor *sensor) { this->calibrated_ = sensor; }

 protected:
  void update_();

  MotionblindsBLEMotor *motor_{nullptr};
  binary_sensor::BinarySensor *fresh_{nullptr};
  binary_sensor::BinarySensor *calibrated_{nullptr};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
