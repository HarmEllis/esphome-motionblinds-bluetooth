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
  for (const bool downward : {false, true}) {
    Blind b;
    b.top.position = downward ? 20 : 70;
    b.bottom.position = downward ? 30 : 80;
    b.coordinator.set_rail_position(Rail::TOP, downward ? 0.4f : 0.9f);
    b.coordinator.set_rail_position(Rail::BOTTOM, downward ? 0.1f : 0.6f);
    auto &lead = downward ? b.bottom : b.top;
    auto &trail = downward ? b.top : b.bottom;
    assert(lead.commands == 1 && trail.commands == 0);

    b.coordinator.loop();
    assert(trail.commands == 0); // a queued write is not departure
    lead.written = true;
    b.coordinator.loop();
    assert(trail.commands == 0); // a sent write is not departure either
    const float origin = lead.position;
    lead.position = origin + (downward ? -1 : 1);
    b.coordinator.loop();
    assert(trail.commands == 0); // movement toward the other rail is not proof
    lead.position = origin + (downward ? 1 : -1);
    lead.fresh = false;
    b.coordinator.loop();
    assert(trail.commands == 0); // require feedback on the current link
    lead.fresh = true;
    b.coordinator.loop();
    assert(lead.pending && trail.commands == 1); // follow before the leader finishes
    assert(trail.target == lead.position); // never command past observed clearance

    trail.complete(trail.target);
    b.coordinator.loop();
    assert(trail.commands == 1); // do not finish the clipped target while the leader is busy
    lead.complete(downward ? 90 : 10);
    b.coordinator.loop();
    assert(trail.commands == 2 && trail.target == (downward ? 60 : 40));
    trail.complete(trail.target);
    b.coordinator.loop();
    assert(std::strcmp(b.coordinator.status_text(), "idle") == 0);
    assert(b.top.leases == 0 && b.bottom.leases == 0);
  }
  {
    Blind first, second;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.loop();
    assert(first.bottom.commands == 1 && second.bottom.commands == 1);
    assert(first.top.commands == 0 && second.top.commands == 0);
    assert(first.bottom.pending && second.bottom.pending);
  }
  {
    Blind first, second;
    // Both HA calls arrive, but top is already at its target: only bottom moves.
    first.coordinator.set_rail_position(Rail::TOP, 1.0f);
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.set_rail_position(Rail::TOP, 1.0f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(first.bottom.commands == 1 && second.bottom.commands == 1);
    assert(first.top.commands == 0 && second.top.commands == 0);
  }
  {
    Blind first, second;
    first.bottom.position = 100;
    first.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.loop();
    assert(first.top.commands == 1 && second.bottom.commands == 1);
    assert(first.bottom.commands == 0 && second.top.commands == 0);
  }
  {
    Blind first, second;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.set_rail_position(Rail::TOP, 0.5f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(second.bottom.commands == 0);
    // Replacing the queued top target makes this a single-rail move, so it
    // may now start alongside the other blind instead of keeping its old turn.
    second.coordinator.set_rail_position(Rail::TOP, 1.0f);
    assert(first.bottom.pending && second.bottom.commands == 1);
    assert(second.top.commands == 0);
  }
  {
    Blind first, second;
    // A combined cover request can also need only one motor.
    first.bottom.position = second.bottom.position = 80;
    first.coordinator.set_combined_openness(0);
    second.coordinator.set_combined_openness(0);
    assert(first.bottom.commands == 1 && second.bottom.commands == 1);
    assert(first.top.commands == 0 && second.top.commands == 0);
  }
  {
    Blind first, second, paired, newer;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.loop();
    paired.coordinator.set_rail_position(Rail::TOP, 0.5f);
    paired.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    assert(paired.bottom.commands == 0);
    newer.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    newer.coordinator.loop();
    assert(newer.bottom.commands == 0); // single moves cannot starve an older pair

    first.bottom.complete(80);
    first.coordinator.loop();
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    paired.coordinator.loop();
    assert(paired.bottom.commands == 0); // second single move is still active
    second.bottom.complete(80);
    second.coordinator.loop();
    paired.coordinator.loop();
    assert(paired.bottom.commands == 0); // second link must also disconnect
    second.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    paired.coordinator.loop();
    assert(paired.bottom.commands == 1);
    newer.coordinator.loop();
    assert(newer.bottom.commands == 0); // a paired move keeps its exclusive turn
    paired.bottom.complete(80);
    paired.coordinator.loop();
    paired.top.complete(50);
    paired.coordinator.loop();
    newer.coordinator.loop();
    assert(newer.bottom.commands == 0); // the pair's links are still open
    paired.top.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    paired.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    newer.coordinator.loop();
    assert(newer.bottom.commands == 1); // the waiting single move eventually runs
  }
  {
    Blind first, second;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.prepare();
    assert(second.top.leases == 0 && second.bottom.leases == 0);
    first.bottom.complete(80);
    first.coordinator.loop();
    first.bottom.current_state = esphome::motionblinds_ble::MotorState::IDLE;
    second.coordinator.loop();
    assert(second.top.leases == 1 && second.bottom.leases == 1);
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.1f);
    esphome::test_now += 31;
    first.coordinator.loop();
    assert(first.bottom.commands == 1); // preparation also owns both links
  }
  {
    Blind first, second;
    first.top.fresh = false;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    assert(first.top.leases == 1 && first.bottom.leases == 1);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    second.coordinator.loop();
    assert(second.bottom.commands == 0); // a refresh needs both links
  }
  {
    Blind first, second;
    first.coordinator.set_optimistic(true);
    second.coordinator.set_optimistic(true);
    first.top.fresh = first.bottom.fresh = false;
    second.top.fresh = second.bottom.fresh = false;
    first.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    second.coordinator.set_rail_position(Rail::BOTTOM, 0.2f);
    esphome::test_now += 31;
    first.coordinator.loop();
    second.coordinator.loop();
    assert(first.bottom.commands == 1 && second.bottom.commands == 1);
    assert(first.top.leases == 0 && second.top.leases == 0);
  }
  {
    Blind b;
    b.top.speed_level = 1;
    b.bottom.speed_level = 2;
    b.top.position = 20;
    b.bottom.position = 30;
    b.coordinator.set_rail_position(Rail::TOP, 0.4f);
    b.coordinator.set_rail_position(Rail::BOTTOM, 0.1f);
    b.bottom.written = true;
    b.bottom.position = 31;
    b.coordinator.loop();
    assert(b.top.commands == 0); // a speed mismatch still requires the full target
    b.bottom.position = 60;
    b.coordinator.loop();
    assert(b.bottom.pending && b.top.commands == 1 && b.top.target == 60);
  }
  {
    Blind b;
    b.twilight();
    b.bottom.position = 1;
    b.coordinator.loop();
    assert(b.top.commands == 1 && b.top.target == 1);
    b.bottom.fail(); // the follower has already started when the leader fails
    b.coordinator.loop();
    assert(b.top.stops == 1 && !b.top.pending);
    assert(std::strstr(b.coordinator.status_text(), "bottom rail failed"));
    assert(b.top.leases == 0 && b.bottom.leases == 0);
  }
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
