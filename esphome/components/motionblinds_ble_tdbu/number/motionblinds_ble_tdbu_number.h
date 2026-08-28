#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"

#include "../motionblinds_ble_tdbu.h"

namespace esphome::motionblinds_ble_tdbu {

/// Slides the fabric block up and down without changing how much of the window
/// it covers.
///
/// This is a separate entity because it is a separate physical quantity from
/// openness. Folding both into one cover, as the Home Assistant integration
/// does, produces a slider whose value means "where the fabric is" while its
/// open and close buttons mean "how much fabric shows" — so a blind parked at
/// the top and a blind covering the whole window can report the same position.
class MotionblindsBLETdbuNumber : public number::Number, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_tdbu(MotionblindsBLETdbu *tdbu) { this->tdbu_ = tdbu; }

 protected:
  void control(float value) override;
  void update_state_();

  MotionblindsBLETdbu *tdbu_{nullptr};
};

}  // namespace esphome::motionblinds_ble_tdbu

#endif  // USE_ESP32
