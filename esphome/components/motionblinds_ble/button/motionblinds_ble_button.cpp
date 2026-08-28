#include "motionblinds_ble_button.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble.button";

void MotionblindsBLEButton::dump_config() { LOG_BUTTON("", "Motionblinds BLE Button", this); }

void MotionblindsBLEButton::press_action() {
  if (this->motor_ == nullptr)
    return;

  switch (this->action_) {
    case ButtonAction::STATUS_QUERY:
      this->motor_->request_status();
      break;
    case ButtonAction::FAVORITE:
      // The motor reports only whether a favorite exists, never where it is,
      // so a coordinator cannot clamp this move. On a top-down bottom-up
      // blind the clearance watchdog is the only thing standing between this
      // button and a collision.
      this->motor_->request_favorite();
      break;
    case ButtonAction::CONNECT:
      this->motor_->request_connect();
      break;
    case ButtonAction::DISCONNECT:
      this->motor_->request_disconnect();
      break;
  }
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
