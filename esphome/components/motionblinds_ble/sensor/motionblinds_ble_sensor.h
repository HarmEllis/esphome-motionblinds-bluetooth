#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble.h"

namespace esphome::motionblinds_ble {

/// Diagnostics for one motor. Battery arrives free with every status frame,
/// which is the only moment these motors say anything about themselves.
class MotionblindsBLESensor : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motor(MotionblindsBLEMotor *motor) { this->motor_ = motor; }
  void set_battery_sensor(sensor::Sensor *sensor) { this->battery_ = sensor; }
  void set_signal_sensor(sensor::Sensor *sensor) { this->signal_ = sensor; }

 protected:
  void update_();

  MotionblindsBLEMotor *motor_{nullptr};
  sensor::Sensor *battery_{nullptr};
  sensor::Sensor *signal_{nullptr};
};

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
