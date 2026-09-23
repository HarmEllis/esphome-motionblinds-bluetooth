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
  std::puts("coordinator outcomes: all checks passed");
}
