#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/component.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

#include "motionblinds_ble_tdbu.h"

namespace esphome::motionblinds_ble_tdbu {

/* The per-rail diagnostics of one blind, created from a single `diagnostics:`
 * block on the coordinator.
 *
 * These are exactly the entities you want for every rail — is the battery
 * running down, is the radio reaching it, is it connected, and is the position
 * it reports something it actually observed. Declaring them by hand means
 * twenty entities of near-identical YAML for a three-blind window, so the
 * coordinator builds them itself: it already knows both motors.
 *
 * The individual `motionblinds_ble` platforms remain available for anyone who
 * wants a different subset, different names, or diagnostics on a motor that is
 * not part of a top-down bottom-up blind.
 */
class MotionblindsBLETdbuDiagnostics : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_tdbu(MotionblindsBLETdbu *tdbu) { this->tdbu_ = tdbu; }

#ifdef USE_SENSOR
  void set_battery(Rail rail, sensor::Sensor *sensor) { this->rail_(rail).battery = sensor; }
  void set_signal(Rail rail, sensor::Sensor *sensor) { this->rail_(rail).signal = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_position_fresh(Rail rail, binary_sensor::BinarySensor *sensor) { this->rail_(rail).fresh = sensor; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_connection(Rail rail, text_sensor::TextSensor *sensor) { this->rail_(rail).connection = sensor; }
  void set_status(text_sensor::TextSensor *sensor) { this->status_ = sensor; }
#endif

 protected:
  struct RailEntities {
#ifdef USE_SENSOR
    sensor::Sensor *battery{nullptr};
    sensor::Sensor *signal{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
    binary_sensor::BinarySensor *fresh{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
    text_sensor::TextSensor *connection{nullptr};
#endif
  };

  RailEntities &rail_(Rail rail) { return rail == Rail::TOP ? this->top_ : this->bottom_; }
  void publish_(Rail rail);
  void publish_status_();

  MotionblindsBLETdbu *tdbu_{nullptr};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_{nullptr};
#endif
  RailEntities top_;
  RailEntities bottom_;
};

#ifdef USE_BUTTON
/// Asks one rail's motor for a fresh status frame.
///
/// This is the counterpart of the update_entity call that is the only thing
/// known to wake a motor which connects and then silently ignores commands.
class MotionblindsBLETdbuRefreshButton : public button::Button, public Component {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_tdbu(MotionblindsBLETdbu *tdbu) { this->tdbu_ = tdbu; }
  void set_rail(Rail rail) { this->rail_ = rail; }

 protected:
  void press_action() override;

  MotionblindsBLETdbu *tdbu_{nullptr};
  Rail rail_{Rail::TOP};
};

/// Opens and keys both motor links ahead of a time-critical movement.
class MotionblindsBLETdbuPrepareButton : public button::Button, public Component {
 public:
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_tdbu(MotionblindsBLETdbu *tdbu) { this->tdbu_ = tdbu; }

 protected:
  void press_action() override;
  MotionblindsBLETdbu *tdbu_{nullptr};
};
#endif

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
