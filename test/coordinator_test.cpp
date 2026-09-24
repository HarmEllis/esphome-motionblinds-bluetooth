#include <cassert>
#include <cstring>
#include <cstdio>
#include "esphome/core/hal.h"
#include "motionblinds_ble_tdbu.h"

using namespace esphome::motionblinds_ble_tdbu;

struct Blind {
  MotionblindsBLEMotor top, bottom;
  MotionblindsBLETdbu coordinator;
  Blind() {
    coordinator.set_top_motor(&top);
    coordinator.set_bottom_motor(&bottom);
    coordinator.setup();
  }
  void twilight() {
    coordinator.set_rail_position(Rail::TOP, 0.5f);
    coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(bottom.commands == 1 && top.commands == 0);
    bottom.written = true;
    coordinator.loop(); // preconnect top before bottom finishes
    assert(top.leases == 1);
  }
};

int main() {
  {
    Blind b;
    b.twilight();
    b.bottom.complete(80);
    b.coordinator.loop();
    assert(b.top.commands == 1);
    b.top.fail(); // recorded bank-top incident: queue empty after failure
    b.coordinator.loop();
    assert(std::strstr(b.coordinator.status_text(), "top rail failed"));
    assert(b.top.leases == 0 && b.bottom.leases == 0);
    b.coordinator.loop();
    assert(b.top.commands == 1); // no residual/restart after failure
  }
  {
    Blind b;
    b.twilight();
    b.top.fail(); // preconnect fails while bottom is still moving
    b.coordinator.loop();
    assert(b.bottom.stops == 1);
    assert(b.top.commands == 0); // no follower dispatch after failure
    assert(std::strstr(b.coordinator.status_text(), "top rail failed"));
  }
  {
    Blind b;
    b.twilight();
    b.bottom.fail();
    b.coordinator.loop();
    assert(b.top.commands == 0);
    assert(std::strstr(b.coordinator.status_text(), "bottom rail failed"));
  }
  for (float final_position : {0.0f, 50.0f}) {
    Blind b;
    b.twilight();
    b.bottom.complete(80);
    b.coordinator.loop();
    b.top.complete(final_position);
    b.coordinator.loop();
    if (final_position == 50)
      assert(std::strcmp(b.coordinator.status_text(), "idle") == 0);
    else
      assert(std::strstr(b.coordinator.status_text(), "top rail did not reach"));
    assert(b.top.leases == 0 && b.bottom.leases == 0);
  }
  {
    Blind first, second;
    first.twilight();
    second.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(second.bottom.commands == 0 && second.top.commands == 0);
    assert(std::strstr(second.coordinator.status_text(), "BLE turn"));

    // A newer target for a queued rail replaces the older one before either
    // motor is touched. The pair is planned against positions at its own turn.
    second.coordinator.set_rail_position(Rail::TOP, 0.4f);
    first.bottom.complete(80);
    first.coordinator.loop();
    first.top.complete(50);
    first.coordinator.loop();
    second.coordinator.loop();
    assert(second.bottom.commands == 0); // first pair still holds live links
    first.top.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    second.coordinator.loop();
    assert(second.bottom.commands == 1 && second.top.commands == 0);
    second.bottom.complete(80);
    second.coordinator.loop();
    assert(second.top.commands == 1 && second.top.target == 60);
  }
  {
    Blind first, second;
    first.twilight();
    second.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.stop_all();
    assert(second.top.stops == 0 && second.bottom.stops == 0);
    first.bottom.complete(80);
    first.coordinator.loop();
    first.top.fail();
    first.coordinator.loop();
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    second.coordinator.loop();
    assert(second.bottom.commands == 0 && second.top.commands == 0);
  }
  {
    Blind first, second;
    first.twilight();
    second.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    first.bottom.complete(80);
    first.coordinator.loop();
    first.top.fail();
    first.coordinator.loop();
    second.coordinator.loop();
    assert(second.bottom.commands == 0); // the first bottom link is still open
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    second.coordinator.loop();
    assert(second.bottom.commands == 1); // failure did not discard the next blind
  }
  {
    Blind first, second;
    first.twilight();
    second.coordinator.prepare();
    assert(second.top.leases == 0 && second.bottom.leases == 0);
    first.bottom.complete(80);
    first.coordinator.loop();
    first.top.complete(50);
    first.coordinator.loop();
    first.top.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    second.coordinator.loop();
    assert(second.top.leases == 1 && second.bottom.leases == 1);
    second.coordinator.prepare(); // extending a preparation must not double-lease
    assert(second.top.leases == 1 && second.bottom.leases == 1);
  }
  {
    Blind first, second;
    // The first blind is waiting out the 30 ms rail-pair window. A complete
    // request on the second blind must not jump ahead in that interval.
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(first.bottom.commands == 0 && second.bottom.commands == 0);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.loop();
    assert(first.bottom.commands == 1 && second.bottom.commands == 0);
  }
  std::puts("coordinator outcomes: all checks passed");
}
