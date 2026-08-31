// Host test for the top-down bottom-up geometry.
//
// Build and run:
//   g++ -std=c++17 -O2 -Wall -Wextra -I . -I esphome/components/motionblinds_ble_tdbu
//       test/tdbu_test.cpp -o tdbu_test && ./tdbu_test
//
// The point of this file is the exhaustive sweeps at the bottom: for every
// reachable pair of rail positions and every request a user can make, the
// command actually emitted must keep the rails apart. A rule that only holds
// for the cases someone thought to write down is not a safety guarantee.

#include <cstdio>
#include <string>

#include "motionblinds_tdbu_dispatch.h"
#include "motionblinds_tdbu_geometry.h"

using namespace esphome::motionblinds_ble_tdbu;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &what) {
  g_checks++;
  if (!condition) {
    g_failures++;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

void check_close(float actual, float expected, const std::string &what, float tolerance = 0.01f) {
  g_checks++;
  if (std::fabs(actual - expected) > tolerance) {
    g_failures++;
    std::printf("  FAIL: %s (got %.3f, expected %.3f)\n", what.c_str(), actual, expected);
  }
}

Geometry full(Fabric fabric, float min_gap = 5.0f, float margin = 0.0f) {
  return Geometry(fabric, min_gap, margin, RailRange{0.0f, 100.0f, false}, RailRange{0.0f, 100.0f, false});
}

void test_rail_range_transform() {
  std::printf("rail range transform\n");
  const RailRange plain{0.0f, 100.0f, false};
  check_close(plain.to_window(0.0f), 0.0f, "plain raw 0");
  check_close(plain.to_window(100.0f), 100.0f, "plain raw 100");

  // An upside-down motor runs the other way through the same window.
  const RailRange inverted{0.0f, 100.0f, true};
  check_close(inverted.to_window(0.0f), 100.0f, "inverted raw 0");
  check_close(inverted.to_window(100.0f), 0.0f, "inverted raw 100");

  // A rail calibrated over only part of the window.
  const RailRange partial{20.0f, 60.0f, false};
  check_close(partial.to_window(0.0f), 20.0f, "partial raw 0");
  check_close(partial.to_window(100.0f), 60.0f, "partial raw 100");
  check_close(partial.to_window(50.0f), 40.0f, "partial raw 50");

  // Round-tripping must be exact, otherwise every target drifts.
  for (const RailRange &range : {plain, inverted, partial}) {
    for (int raw = 0; raw <= 100; raw++) {
      const float window = range.to_window(static_cast<float>(raw));
      check_close(range.to_raw(window), static_cast<float>(raw), "transform round trip", 0.01f);
    }
  }
  check(!RailRange({60.0f, 20.0f, false}).valid(), "reversed range rejected");
  check(!RailRange({0.0f, 101.0f, false}).valid(), "out-of-window range rejected");
}

void test_openness_direction() {
  std::printf("openness direction\n");
  const Geometry between = full(Fabric::BETWEEN_RAILS);
  const Geometry outside = full(Fabric::OUTSIDE_IN);

  // Fabric spanning the whole window is closed; collapsed against itself is open.
  check_close(between.length_to_openness(100.0f), 0.0f, "between_rails full fabric is closed");
  check_close(between.length_to_openness(5.0f), 1.0f, "between_rails collapsed is open");

  // Rails meeting in the middle is closed; parked at the edges is open.
  check_close(outside.length_to_openness(5.0f), 0.0f, "outside_in rails together is closed");
  check_close(outside.length_to_openness(100.0f), 1.0f, "outside_in rails apart is open");

  // The two fabrics are exact opposites of each other for the same geometry.
  for (int length = 5; length <= 100; length++) {
    const float l = static_cast<float>(length);
    check_close(between.length_to_openness(l) + outside.length_to_openness(l), 1.0f, "fabrics are complementary");
  }

  for (const Geometry &geometry : {between, outside}) {
    for (int percent = 0; percent <= 100; percent++) {
      const float openness = static_cast<float>(percent) / 100.0f;
      check_close(geometry.length_to_openness(geometry.openness_to_length(openness)), openness,
                  "openness round trip");
    }
  }
}

void test_placement_keeps_extremes_reachable() {
  std::printf("placement\n");
  const Geometry geometry = full(Fabric::BETWEEN_RAILS);

  const Placement closed = geometry.place_segment(geometry.openness_to_length(0.0f), 50.0f);
  check(closed.feasible, "fully closed is feasible");
  check_close(closed.top, 0.0f, "fully closed top");
  check_close(closed.bottom, 100.0f, "fully closed bottom");

  // Asking for fully open from an off-centre position must still give fully
  // open; the centre shifts rather than the request being denied.
  const Placement open = geometry.place_segment(geometry.openness_to_length(1.0f), 2.0f);
  check(open.feasible, "fully open is feasible from near the top");
  check_close(open.bottom - open.top, 5.0f, "fully open length");
  check(open.top >= 0.0f && open.bottom <= 100.0f, "fully open stays in the window");

  // Partially calibrated rails genuinely cannot span the window, and the
  // scales are normalised against what they can do.
  const Geometry partial(Fabric::BETWEEN_RAILS, 5.0f, 0.0f, RailRange{10.0f, 70.0f, false},
                         RailRange{30.0f, 90.0f, false});
  check_close(partial.max_length(), 80.0f, "partial max length");
  const Placement partial_closed = partial.place_segment(partial.openness_to_length(0.0f), 50.0f);
  check(partial_closed.feasible, "partial fully closed is feasible");
  check_close(partial_closed.top, 10.0f, "partial closed top");
  check_close(partial_closed.bottom, 90.0f, "partial closed bottom");

  // Ranges that do not overlap force a minimum separation of their own.
  const Geometry disjoint(Fabric::OUTSIDE_IN, 5.0f, 0.0f, RailRange{0.0f, 30.0f, false},
                          RailRange{60.0f, 100.0f, false});
  check_close(disjoint.min_length(), 30.0f, "disjoint ranges force a wider minimum than min_gap");
  const Placement squeezed = disjoint.place_segment(5.0f, 50.0f);
  check(!squeezed.feasible, "over-short request reported as not achieved");
  check(squeezed.bottom - squeezed.top >= 30.0f - 0.01f, "over-short request still respects the ranges");
}

void test_classification() {
  std::printf("direction classification\n");
  // The top rail moving up and the bottom rail moving down both open the gap.
  check(Geometry::classify(Rail::TOP, 50.0f, 20.0f) == Direction::AWAY, "top upward is away");
  check(Geometry::classify(Rail::TOP, 20.0f, 50.0f) == Direction::TOWARD, "top downward is toward");
  check(Geometry::classify(Rail::BOTTOM, 50.0f, 80.0f) == Direction::AWAY, "bottom downward is away");
  check(Geometry::classify(Rail::BOTTOM, 80.0f, 50.0f) == Direction::TOWARD, "bottom upward is toward");
  check(Geometry::classify(Rail::TOP, 50.0f, 50.2f) == Direction::STATIONARY, "sub-quantum move is stationary");
  check(Geometry::classify(Rail::BOTTOM, 50.0f, 50.2f) == Direction::STATIONARY, "sub-quantum move is stationary");
}

void test_clamping() {
  std::printf("clamping\n");
  const Geometry geometry = full(Fabric::BETWEEN_RAILS, 5.0f, 2.0f);
  // Driving the top rail into the bottom one is clamped to the clearance.
  check_close(geometry.clamp_target(Rail::TOP, 90.0f, 40.0f), 33.0f, "top clamped to gap + margin");
  check_close(geometry.clamp_target(Rail::BOTTOM, 10.0f, 40.0f), 47.0f, "bottom clamped to gap + margin");
  // A move that is already safe is left alone.
  check_close(geometry.clamp_target(Rail::TOP, 10.0f, 40.0f), 10.0f, "safe target untouched");
}

void test_rail_request_pairing_window() {
  std::printf("rail request pairing window\n");
  constexpr uint32_t window = 30;

  check(!rail_request_ready(false, true, false, 0, window), "first top request waits for its pair");
  check(!rail_request_ready(false, false, true, window - 1, window), "single bottom still waits inside window");
  check(rail_request_ready(false, true, false, window, window), "single rail dispatches at window expiry");
  check(rail_request_ready(false, true, true, 1, window), "paired rails dispatch without waiting out window");
  check(rail_request_ready(true, false, false, 0, window), "combined request dispatches immediately");

  // Regression for two rails stacked at the head: top-to-50 arrives first,
  // bottom-to-20 second. Planning either request alone makes top stationary;
  // planning the pair correctly chooses the gap-opening bottom as leader.
  const Geometry geometry = full(Fabric::BETWEEN_RAILS, 0.0f, 0.0f);
  const float top = 0.0f;
  const float bottom = 0.0f;
  const float target_top = geometry.rail_window_target(Rail::TOP, 0.50f);
  const float target_bottom = geometry.rail_window_target(Rail::BOTTOM, 0.20f);
  const Placement paired = geometry.place_segment(target_bottom - target_top, (target_top + target_bottom) / 2.0f);
  check(paired.feasible, "TV schemer pair is geometrically feasible");
  check_close(paired.top, 50.0f, "paired top keeps its requested target");
  check_close(paired.bottom, 80.0f, "paired bottom keeps its requested target");

  DispatchRequest request;
  request.top = Geometry::classify(Rail::TOP, top, paired.top);
  request.bottom = Geometry::classify(Rail::BOTTOM, bottom, paired.bottom);
  request.gap = bottom - top;
  request.start_gap = 10.0f;
  const DispatchPlan plan = plan_dispatch(request);
  check(plan.moves && plan.has_trail, "both rails remain part of the plan");
  check(plan.lead == Rail::BOTTOM, "gap-opening bottom rail leads the stacked pair");
  check(plan.rule == StartRule::AFTER_DEPARTURE, "top follows after observed bottom departure");
}

// Every raw command the component can emit, from every state, must leave the
// rails at least min_gap apart.
void test_exhaustive_invariant() {
  std::printf("exhaustive invariant sweep\n");
  const Geometry geometries[] = {
      // The defaults: rails that are allowed to stack against each other. The
      // invariant then reduces to "the bottom rail is never above the top one",
      // which is the one thing that must still hold.
      full(Fabric::BETWEEN_RAILS, 0.0f, 0.0f),
      full(Fabric::OUTSIDE_IN, 0.0f, 0.0f),
      Geometry(Fabric::BETWEEN_RAILS, 0.0f, 0.0f, RailRange{0.0f, 100.0f, true}, RailRange{0.0f, 100.0f, false}),
      full(Fabric::BETWEEN_RAILS, 5.0f, 2.0f),
      full(Fabric::OUTSIDE_IN, 5.0f, 2.0f),
      Geometry(Fabric::BETWEEN_RAILS, 5.0f, 2.0f, RailRange{0.0f, 100.0f, true}, RailRange{0.0f, 100.0f, false}),
      Geometry(Fabric::OUTSIDE_IN, 5.0f, 2.0f, RailRange{0.0f, 100.0f, false}, RailRange{0.0f, 100.0f, true}),
      Geometry(Fabric::BETWEEN_RAILS, 5.0f, 2.0f, RailRange{10.0f, 70.0f, true}, RailRange{30.0f, 90.0f, true}),
      Geometry(Fabric::OUTSIDE_IN, 8.0f, 3.0f, RailRange{0.0f, 55.0f, false}, RailRange{45.0f, 100.0f, false}),
  };

  int violations = 0;
  int rail_moves = 0;
  int combined_moves = 0;

  for (const Geometry &geometry : geometries) {
    check(geometry.valid(), "sweep geometry is valid");

    for (int top_raw = 0; top_raw <= 100; top_raw += 1) {
      const float top = geometry.top_range().to_window(static_cast<float>(top_raw));
      for (int bottom_raw = 0; bottom_raw <= 100; bottom_raw += 1) {
        const float bottom = geometry.bottom_range().to_window(static_cast<float>(bottom_raw));
        if (bottom - top < geometry.min_gap())
          continue;  // start states that already violate the invariant

        for (int percent = 0; percent <= 100; percent += 5) {
          const float openness = static_cast<float>(percent) / 100.0f;

          // Single-rail moves: the other rail stays where it is.
          const float top_target = geometry.rail_window_target(Rail::TOP, openness);
          const uint8_t top_cmd = geometry.raw_target(Rail::TOP, top_target, bottom);
          const float top_result = geometry.top_range().to_window(static_cast<float>(top_cmd));
          rail_moves++;
          if (bottom - top_result < geometry.min_gap() - 0.001f)
            violations++;

          const float bottom_target = geometry.rail_window_target(Rail::BOTTOM, openness);
          const uint8_t bottom_cmd = geometry.raw_target(Rail::BOTTOM, bottom_target, top);
          const float bottom_result = geometry.bottom_range().to_window(static_cast<float>(bottom_cmd));
          rail_moves++;
          if (bottom_result - top < geometry.min_gap() - 0.001f)
            violations++;

          // Combined moves: both rails get a target from one placement.
          const Placement placement =
              geometry.place_segment(geometry.openness_to_length(openness), (top + bottom) / 2.0f);
          const uint8_t combined_top = geometry.raw_target(Rail::TOP, placement.top, placement.bottom);
          const uint8_t combined_bottom = geometry.raw_target(Rail::BOTTOM, placement.bottom, placement.top);
          const float ct = geometry.top_range().to_window(static_cast<float>(combined_top));
          const float cb = geometry.bottom_range().to_window(static_cast<float>(combined_bottom));
          combined_moves++;
          if (cb - ct < geometry.min_gap() - 0.001f)
            violations++;
        }
      }
    }
  }

  std::printf("  swept %d single-rail and %d combined moves\n", rail_moves, combined_moves);
  check(violations == 0, "no commanded target violates min_gap (" + std::to_string(violations) + " violations)");
}

// Asking for the extremes must actually reach them, or the entity is lying
// about its own scale.
// A rail's reported position must depend only on that rail. The old scaled
// version moved a stationary rail's reading whenever the other one travelled.
void test_rail_position_is_absolute() {
  std::printf("absolute rail positions\n");
  const Geometry geometry = full(Fabric::BETWEEN_RAILS, 0.0f, 0.0f);

  // With rails that may meet, both ends of the window are reachable for each
  // rail no matter where the other one is: a blind collapsed at the head reads
  // 100 on both rails, not 100 and 93.
  check_close(geometry.clamp_target(Rail::BOTTOM, 0.0f, 0.0f), 0.0f, "bottom may join the top rail at the head");
  check_close(geometry.clamp_target(Rail::TOP, 100.0f, 100.0f), 100.0f, "top may join the bottom rail at the sill");
  // Crossing is still refused.
  check_close(geometry.clamp_target(Rail::BOTTOM, 0.0f, 40.0f), 40.0f, "bottom may not rise above the top rail");
  check_close(geometry.clamp_target(Rail::TOP, 100.0f, 40.0f), 40.0f, "top may not sink below the bottom rail");

  // Both rails report how high they are, so the up arrow means up either way.
  check_close(geometry.rail_position(Rail::TOP, 0.0f), 1.0f, "top raised reads 1");
  check_close(geometry.rail_position(Rail::TOP, 100.0f), 0.0f, "top lowered reads 0");
  check_close(geometry.rail_position(Rail::TOP, 50.0f), 0.5f, "top halfway");

  check_close(geometry.rail_position(Rail::BOTTOM, 100.0f), 0.0f, "bottom down at the sill reads 0");
  check_close(geometry.rail_position(Rail::BOTTOM, 0.0f), 1.0f, "bottom raised reads 1");
  check_close(geometry.rail_position(Rail::BOTTOM, 50.0f), 0.5f, "bottom halfway");

  // Round trip through the inverse.
  for (const Rail rail : {Rail::TOP, Rail::BOTTOM}) {
    for (int percent = 0; percent <= 100; percent++) {
      const float position = static_cast<float>(percent) / 100.0f;
      const float window = geometry.rail_window_target(rail, position);
      check_close(geometry.rail_position(rail, window), position, "rail position round trip");
    }
  }

  // A partially calibrated rail is scaled against its own travel only.
  const Geometry partial(Fabric::BETWEEN_RAILS, 5.0f, 0.0f, RailRange{20.0f, 60.0f, false},
                         RailRange{40.0f, 90.0f, false});
  check_close(partial.rail_position(Rail::TOP, 20.0f), 1.0f, "partial top raised to its own head");
  check_close(partial.rail_position(Rail::TOP, 60.0f), 0.0f, "partial top at its own lowest");
  check_close(partial.rail_position(Rail::BOTTOM, 90.0f), 0.0f, "partial bottom down at its own sill");

  // An inverted motor still reports the same physical thing.
  const Geometry inverted(Fabric::BETWEEN_RAILS, 5.0f, 0.0f, RailRange{0.0f, 100.0f, true},
                          RailRange{0.0f, 100.0f, false});
  check_close(inverted.rail_position(Rail::TOP, 0.0f), 1.0f, "inverted top raised still reads 1");
  check_close(inverted.rail_position(Rail::TOP, 100.0f), 0.0f, "inverted top lowered still reads 0");
}

void test_extremes_are_reachable() {
  std::printf("extremes\n");
  const Geometry geometries[] = {
      full(Fabric::BETWEEN_RAILS, 0.0f, 0.0f),
      full(Fabric::OUTSIDE_IN, 0.0f, 0.0f),
      full(Fabric::BETWEEN_RAILS, 5.0f, 0.0f),
      full(Fabric::OUTSIDE_IN, 5.0f, 0.0f),
      Geometry(Fabric::BETWEEN_RAILS, 5.0f, 0.0f, RailRange{10.0f, 70.0f, false}, RailRange{30.0f, 90.0f, false}),
  };

  for (const Geometry &geometry : geometries) {
    for (int percent : {0, 100}) {
      const float openness = static_cast<float>(percent) / 100.0f;
      const Placement placement = geometry.place_segment(geometry.openness_to_length(openness), 50.0f);
      check(placement.feasible, "extreme is feasible");
      check_close(geometry.length_to_openness(placement.bottom - placement.top), openness,
                  "extreme reports back as asked", 0.02f);
    }

  }
}

// --------------------------------------------------------------- dispatch

/// The window position a rail is actually commanded to, which is the target
/// after clamping and after the outward rounding of whole-numbered feedback.
float commanded(const Geometry &geometry, Rail rail, float target, float other) {
  const RailRange &range = rail == Rail::TOP ? geometry.top_range() : geometry.bottom_range();
  return range.to_window(static_cast<float>(geometry.raw_target(rail, target, other)));
}

DispatchRequest request(Direction top, Direction bottom, float gap, float start_gap) {
  DispatchRequest request;
  request.top = top;
  request.bottom = bottom;
  request.gap = gap;
  request.start_gap = start_gap;
  return request;
}

// The whole point of the direction-aware rule, stated as a table. Two rails
// opening the gap can always start together; two rails translating keep a
// constant gap and so can never satisfy start_gap; two rails converging keep
// the conservative rule they always had.
void test_start_rule_table() {
  std::printf("start rule\n");

  for (int gap_percent = 0; gap_percent <= 100; gap_percent++) {
    const float gap = static_cast<float>(gap_percent);
    for (const float start_gap : {0.0f, 5.0f, 10.0f, 50.0f}) {
      const bool roomy = gap >= start_gap;

      // Both rails opening the gap: together, whatever start_gap says.
      const DispatchPlan away = plan_dispatch(request(Direction::AWAY, Direction::AWAY, gap, start_gap));
      check(away.has_trail && away.rule == StartRule::TOGETHER, "both rails away start together");
      check(!away.same_direction, "both rails away is not a translation");

      // Both rails closing in: unchanged from the rule that shipped.
      const DispatchPlan toward = plan_dispatch(request(Direction::TOWARD, Direction::TOWARD, gap, start_gap));
      check(toward.lead == Rail::TOP, "both rails toward: the top rail leads by convention");
      check(toward.rule == (roomy ? StartRule::TOGETHER : StartRule::AFTER_CLEARANCE),
            "both rails toward keeps the conservative rule");

      // A translation: the rail opening the gap leads, and below start_gap the
      // follower waits for departure rather than for a gap that never grows.
      const DispatchPlan down = plan_dispatch(request(Direction::TOWARD, Direction::AWAY, gap, start_gap));
      check(down.lead == Rail::BOTTOM && down.trail == Rail::TOP, "translating down: the bottom rail leads");
      check(down.same_direction, "one away and one toward is a translation");
      check(down.rule == (roomy ? StartRule::TOGETHER : StartRule::AFTER_DEPARTURE),
            "a close translation waits for departure");

      const DispatchPlan up = plan_dispatch(request(Direction::AWAY, Direction::TOWARD, gap, start_gap));
      check(up.lead == Rail::TOP && up.trail == Rail::BOTTOM, "translating up: the top rail leads");
      check(up.rule == (roomy ? StartRule::TOGETHER : StartRule::AFTER_DEPARTURE),
            "a close translation waits for departure either way");

      // Known-different speeds take the early release away again: a follower
      // travelling faster closes the gap for the whole move, and no amount of
      // departure proof can see that coming.
      DispatchRequest mismatched = request(Direction::TOWARD, Direction::AWAY, gap, start_gap);
      mismatched.speeds_known_different = true;
      check(plan_dispatch(mismatched).rule == (roomy ? StartRule::TOGETHER : StartRule::AFTER_CLEARANCE),
            "a known speed mismatch falls back to the conservative rule");

      // The escape hatch restores the gap-only rule exactly, in every direction.
      for (const Direction top : {Direction::AWAY, Direction::TOWARD}) {
        for (const Direction bottom : {Direction::AWAY, Direction::TOWARD}) {
          DispatchRequest off = request(top, bottom, gap, start_gap);
          off.direction_aware = false;
          check(plan_dispatch(off).rule == (roomy ? StartRule::TOGETHER : StartRule::AFTER_CLEARANCE),
                "direction_aware: false is the pre-existing rule");
        }
      }
    }
  }

  // A stationary rail is not a second command to hold back.
  for (const Direction moving : {Direction::AWAY, Direction::TOWARD}) {
    const DispatchPlan top_only = plan_dispatch(request(moving, Direction::STATIONARY, 0.0f, 10.0f));
    check(top_only.moves && !top_only.has_trail && top_only.lead == Rail::TOP, "a lone top rail just goes");
    const DispatchPlan bottom_only = plan_dispatch(request(Direction::STATIONARY, moving, 0.0f, 10.0f));
    check(bottom_only.moves && !bottom_only.has_trail && bottom_only.lead == Rail::BOTTOM,
          "a lone bottom rail just goes");
  }
  check(!plan_dispatch(request(Direction::STATIONARY, Direction::STATIONARY, 50.0f, 10.0f)).moves,
        "a blind already there does not move");
}

DepartureEvidence proof(Rail lead, float baseline, float now) {
  DepartureEvidence evidence;
  evidence.lead = lead;
  evidence.lead_fresh = true;
  evidence.baseline_fresh = true;
  evidence.write_sent = true;
  evidence.baseline = baseline;
  evidence.now = now;
  evidence.quantum = 1.0f;
  evidence.gap = 20.0f;
  evidence.effective_gap = 0.0f;
  return evidence;
}

void test_departure_proof() {
  std::printf("departure proof\n");

  // A rail that has not moved has not left.
  check(!departure_proven(proof(Rail::TOP, 50.0f, 50.0f)), "standing still is not departure");
  // Exactly one raw quantum in the away sense is the smallest movement the
  // motors' whole-numbered feedback can possibly show, and it is enough.
  check(departure_proven(proof(Rail::TOP, 50.0f, 49.0f)), "one quantum up proves a top rail left");
  check(!departure_proven(proof(Rail::TOP, 50.0f, 49.5f)), "half a quantum is not enough");
  // Movement the wrong way is never proof, however large.
  check(!departure_proven(proof(Rail::TOP, 50.0f, 60.0f)), "a top rail moving down is not departure");
  check(departure_proven(proof(Rail::BOTTOM, 50.0f, 51.0f)), "one quantum down proves a bottom rail left");
  check(!departure_proven(proof(Rail::BOTTOM, 50.0f, 40.0f)), "a bottom rail moving up is not departure");

  // Every piece of evidence is required, and each on its own withholds release.
  DepartureEvidence evidence = proof(Rail::TOP, 50.0f, 45.0f);
  evidence.lead_fresh = false;
  check(!departure_proven(evidence), "a stale observation is not proof");
  evidence = proof(Rail::TOP, 50.0f, 45.0f);
  evidence.baseline_fresh = false;
  check(!departure_proven(evidence), "a remembered baseline is re-baselined, never counted as movement");
  evidence = proof(Rail::TOP, 50.0f, 45.0f);
  evidence.write_sent = false;
  check(!departure_proven(evidence), "movement before our own write went out is somebody else's");

  // The floor that survives a lead which departs and then stalls.
  evidence = proof(Rail::TOP, 50.0f, 45.0f);
  evidence.gap = 4.0f;
  evidence.effective_gap = 5.0f;
  check(!departure_proven(evidence), "departure below the required clearance still holds the follower");
  evidence.gap = 5.0f;
  check(departure_proven(evidence), "clearance met plus departure releases the follower");

  // A rail with no travel at all has no quantum to move by.
  evidence = proof(Rail::TOP, 50.0f, 0.0f);
  evidence.quantum = 0.0f;
  check(!departure_proven(evidence), "a rail with no range can prove nothing");

  // A partially calibrated rail's quantum is its own, not the window's.
  check_close(rail_quantum(RailRange{0.0f, 100.0f, false}), 1.0f, "a full rail steps by 1%");
  check_close(rail_quantum(RailRange{20.0f, 60.0f, false}), 0.4f, "a rail over 40% of the window steps by 0.4%");
  DepartureEvidence partial = proof(Rail::TOP, 40.0f, 39.6f);
  partial.quantum = rail_quantum(RailRange{20.0f, 60.0f, false});
  check(departure_proven(partial), "one quantum of a partial rail is enough for that rail");
}

// The follower in a translation is initially clamped against the lead's
// observed release position, never against an unacknowledged far-away target.
// Once the lead has really arrived, the residual pass can safely deliver the
// remainder.
void test_translation_is_safe_and_completable() {
  std::printf("translation safety and residual completion\n");

  const Geometry field = full(Fabric::BETWEEN_RAILS, 5.0f, 2.0f);
  // Rails at 10/20 translate down to 50/60. Bottom leads. After one observed
  // percent it is at 21; the top may only be sent to 14 (the 7% effective
  // clearance), not blindly to 50. Once the bottom reaches 60, 50 is safe.
  const float lead_final = commanded(field, Rail::BOTTOM, 60.0f, 10.0f);
  const float trail_first = commanded(field, Rail::TOP, 50.0f, 21.0f);
  const float trail_residual = commanded(field, Rail::TOP, 50.0f, lead_final);
  check_close(lead_final, 60.0f, "the lead receives its full translation target");
  check_close(trail_first, 14.0f, "the early follower stops at the observed clearance floor");
  check_close(trail_residual, 50.0f, "the residual pass reaches the intended target after the lead arrives");

  const Geometry geometries[] = {
      full(Fabric::BETWEEN_RAILS, 0.0f, 0.0f),
      full(Fabric::BETWEEN_RAILS, 5.0f, 2.0f),
      Geometry(Fabric::BETWEEN_RAILS, 5.0f, 2.0f, RailRange{0.0f, 100.0f, true},
               RailRange{0.0f, 100.0f, false}),
      Geometry(Fabric::OUTSIDE_IN, 8.0f, 3.0f, RailRange{0.0f, 55.0f, false},
               RailRange{45.0f, 100.0f, false}),
  };

  int translations = 0;
  int departures = 0;
  int together = 0;
  int stalled_lead_violations = 0;
  int residual_violations = 0;

  for (const Geometry &geometry : geometries) {
    for (int top_raw = 0; top_raw <= 100; top_raw += 2) {
      const float top = geometry.top_range().to_window(static_cast<float>(top_raw));
      for (int bottom_raw = 0; bottom_raw <= 100; bottom_raw += 2) {
        const float bottom = geometry.bottom_range().to_window(static_cast<float>(bottom_raw));
        const float initial_gap = bottom - top;
        if (initial_gap + 0.001f < geometry.effective_gap())
          continue;

        for (int shift = -100; shift <= 100; shift += 5) {
          if (shift == 0)
            continue;
          const float target_top = top + static_cast<float>(shift);
          const float target_bottom = bottom + static_cast<float>(shift);
          if (target_top < geometry.top_range().window_min || target_top > geometry.top_range().window_max ||
              target_bottom < geometry.bottom_range().window_min ||
              target_bottom > geometry.bottom_range().window_max)
            continue;

          const DispatchPlan plan = plan_dispatch(
              request(Geometry::classify(Rail::TOP, top, target_top),
                      Geometry::classify(Rail::BOTTOM, bottom, target_bottom), initial_gap, 10.0f));
          if (!plan.same_direction || !plan.has_trail)
            continue;
          if (plan.rule == StartRule::AFTER_CLEARANCE)
            continue;

          translations++;
          if (plan.rule == StartRule::AFTER_DEPARTURE)
            departures++;
          else
            together++;

          const float lead_start = plan.lead == Rail::TOP ? top : bottom;
          const float trail_start = plan.lead == Rail::TOP ? bottom : top;
          const float lead_target = plan.lead == Rail::TOP ? target_top : target_bottom;
          const float trail_target = plan.lead == Rail::TOP ? target_bottom : target_top;
          const float lead_final = commanded(geometry, plan.lead, lead_target, trail_start);

          float lead_at_release = lead_start;
          if (plan.rule == StartRule::AFTER_DEPARTURE) {
            const RailRange &lead_range = plan.lead == Rail::TOP ? geometry.top_range() : geometry.bottom_range();
            const float clearance_needed = std::fmax(0.0f, geometry.effective_gap() - initial_gap);
            const float departure = std::fmax(rail_quantum(lead_range), clearance_needed);
            if (departure > std::fabs(lead_final - lead_start) + 0.001f)
              continue;  // the lead settles before a concurrent release exists
            lead_at_release = plan.lead == Rail::TOP ? lead_start - departure : lead_start + departure;
          }

          // This mirrors the coordinator: same-direction followers always use
          // the observed release position. If the lead stalls immediately, the
          // follower's complete first command must still preserve the physical
          // minimum gap. Integer motor positions are allowed to consume part of
          // the configured safety margin -- that margin exists for exactly this
          // quantisation and for overshoot -- but may never consume min_gap.
          const float trail_first = commanded(geometry, plan.trail, trail_target, lead_at_release);
          for (const float lead_now : {lead_at_release, lead_final}) {
            for (const float trail_now : {trail_start, trail_first}) {
              const float observed_top = plan.lead == Rail::TOP ? lead_now : trail_now;
              const float observed_bottom = plan.lead == Rail::TOP ? trail_now : lead_now;
              if (observed_bottom - observed_top + 0.001f < geometry.min_gap())
                stalled_lead_violations++;
            }
          }

          // Once the lead has actually arrived, the residual command is
          // recomputed against that observed position and must reach the
          // intended translation without violating the same floor.
          const float trail_residual = commanded(geometry, plan.trail, trail_target, lead_final);
          const float final_top = plan.lead == Rail::TOP ? lead_final : trail_residual;
          const float final_bottom = plan.lead == Rail::TOP ? trail_residual : lead_final;
          const RailRange &trail_range = plan.trail == Rail::TOP ? geometry.top_range() : geometry.bottom_range();
          if (final_bottom - final_top + 0.001f < geometry.effective_gap() ||
              std::fabs(trail_residual - trail_target) > rail_quantum(trail_range) + 0.001f)
            residual_violations++;
        }
      }
    }
  }

  std::printf("  swept %d translations (%d after departure, %d together)\n", translations, departures, together);
  check(translations > 0 && departures > 0 && together > 0, "the sweep exercises both translation start rules");
  check(stalled_lead_violations == 0,
        "a follower stays safe when the lead stalls (" + std::to_string(stalled_lead_violations) +
            " violations)");
  check(residual_violations == 0,
        "the residual reaches every safe translation target (" + std::to_string(residual_violations) +
            " violations)");
}

}  // namespace

int main() {
  test_rail_range_transform();
  test_openness_direction();
  test_placement_keeps_extremes_reachable();
  test_classification();
  test_clamping();
  test_rail_request_pairing_window();
  test_extremes_are_reachable();
  test_rail_position_is_absolute();
  test_exhaustive_invariant();
  test_start_rule_table();
  test_departure_proof();
  test_translation_is_safe_and_completable();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
