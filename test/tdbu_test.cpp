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

// Every raw command the component can emit, from every state, must leave the
// rails at least min_gap apart.
void test_exhaustive_invariant() {
  std::printf("exhaustive invariant sweep\n");
  const Geometry geometries[] = {
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
  const Geometry geometry = full(Fabric::BETWEEN_RAILS, 5.0f, 2.0f);

  // Top rail at the head reads 0, at the sill reads 1, regardless of the other.
  check_close(geometry.rail_position(Rail::TOP, 0.0f), 0.0f, "top at the head");
  check_close(geometry.rail_position(Rail::TOP, 100.0f), 1.0f, "top at the sill");
  check_close(geometry.rail_position(Rail::TOP, 50.0f), 0.5f, "top halfway");

  // Bottom rail: 0 down at the sill, 1 raised to the head.
  check_close(geometry.rail_position(Rail::BOTTOM, 100.0f), 0.0f, "bottom at the sill");
  check_close(geometry.rail_position(Rail::BOTTOM, 0.0f), 1.0f, "bottom at the head");
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
  check_close(partial.rail_position(Rail::TOP, 20.0f), 0.0f, "partial top at its own head");
  check_close(partial.rail_position(Rail::TOP, 60.0f), 1.0f, "partial top at its own end");
  check_close(partial.rail_position(Rail::BOTTOM, 90.0f), 0.0f, "partial bottom at its own sill");

  // An inverted motor still reports the same physical thing.
  const Geometry inverted(Fabric::BETWEEN_RAILS, 5.0f, 0.0f, RailRange{0.0f, 100.0f, true},
                          RailRange{0.0f, 100.0f, false});
  check_close(inverted.rail_position(Rail::TOP, 0.0f), 0.0f, "inverted top at the head still reads 0");
  check_close(inverted.rail_position(Rail::TOP, 100.0f), 1.0f, "inverted top at the sill still reads 1");
}

void test_extremes_are_reachable() {
  std::printf("extremes\n");
  const Geometry geometries[] = {
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

}  // namespace

int main() {
  test_rail_range_transform();
  test_openness_direction();
  test_placement_keeps_extremes_reachable();
  test_classification();
  test_clamping();
  test_extremes_are_reachable();
  test_rail_position_is_absolute();
  test_exhaustive_invariant();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
