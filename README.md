# ESPHome Motionblinds Bluetooth

An [ESPHome](https://esphome.io) component that drives Motionblinds Bluetooth
motors directly from an ESP32, including top-down bottom-up blinds where two
motors share one window and must not be driven into each other.

It replaces the Home Assistant `motionblinds_ble` integration for these motors.
That integration has no deadline anywhere in its connection or command path, so
a failed command can hang indefinitely with no exception, no timeout and
nothing in the log. Here every step has one, and a command that cannot be
verified is reported as a failure rather than as a success.

## What it does differently

- **Nothing waits forever.** Discovery, connecting, the handshake, each command
  and the operation as a whole all have deadlines. Every failure path ends at a
  visible error state, never at silence.
- **A write is not a result.** The command characteristic is written without a
  response, so a successful write proves nothing about the motor. Position
  commands are followed until the motor reports it arrived; the commands that
  cannot be verified are named below rather than assumed to have worked.
- **A remembered position is never trusted.** Positions survive a reboot so the
  entities are not blank, but they are marked stale. A guided move needs a
  position observed during the current connection, from both motors.
- **The connection is held for the whole move.** A rail keeps travelling after
  its command is sent. Disconnecting then would end the position updates that
  the collision watchdog depends on, so both motors are pinned until the blind
  has come to rest.

## Required hardware

- An ESP32 within Bluetooth range of the motors. Range matters more than
  anything else here: motors are reached by waiting for their advertisements.
- **At most six motors per ESP32.** Bluetooth's `GATT_MAX_APPS` is 8 and keeps
  two for its own use, leaving six client registrations. The component rejects
  a seventh at compile time, because ESPHome itself only warns and the build
  would otherwise fail on the ESP during registration.
- For the same reason the node **cannot also run `bluetooth_proxy`** or a
  hand-written `ble_client` when six motors are configured.

## Setup

```yaml
external_components:
  - source: github://HarmEllis/esphome-motionblinds-bluetooth@main
    components: [motionblinds_ble, motionblinds_ble_tdbu]

# Commands are encrypted with a wall-clock timestamp, so the node needs a
# synchronised clock. Without one, commands are refused with an explicit log
# line rather than sent and silently ignored.
time:
  - platform: homeassistant
    id: ha_time

esp32_ble:
  # The number of motors. ESPHome adds the advertising/scanning instance on
  # top of this itself, so do not add one here.
  max_connections: 2

esp32_ble_tracker:
  scan_parameters:
    continuous: true

motionblinds_ble:
  - id: living_top
    mac_address: A4:C1:38:00:0A:5A
    time_id: ha_time
    blind_type: honeycomb
  - id: living_bottom
    mac_address: A4:C1:38:00:EB:AA
    time_id: ha_time
    blind_type: honeycomb
    invert: true

motionblinds_ble_tdbu:
  - id: living_blind
    top_motor: living_top
    bottom_motor: living_bottom
    fabric: between_rails

cover:
  - platform: motionblinds_ble_tdbu
    tdbu_id: living_blind
    rail: combined
    name: "Living blind"
```

A single, non top-down bottom-up motor needs neither `motionblinds_ble_tdbu:`
nor the `rail:` option:

```yaml
cover:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    name: "Office blind"
```

## Motor configuration (`motionblinds_ble:`)

| Variable | Required | Default | Description |
| --- | --- | --- | --- |
| `id` | yes | | Identifier used by the platforms below. |
| `mac_address` | yes | | The motor's address. The Bluetooth client is created for you; there is no `ble_client:` block to write. |
| `time_id` | yes | | A `time:` component. Commands carry a timestamp and are refused while the clock is unsynchronised. |
| `blind_type` | no | `roller` | `roller`, `honeycomb`, `roman`, `venetian`, `double_roller`, `curtain`, `vertical`. Curtain and vertical motors get a longer pause after keying. |
| `invert` | no | `false` | The motor is mounted upside down, so its positions run the other way through the window. |
| `window_min` / `window_max` | no | `0` / `100` | The part of the window this rail actually travels. Needed when the two motors of one blind are each calibrated over their own half. |
| `disconnect_delay` | no | `15s` | Idle time before the connection is dropped. Only starts once no move is in progress. |
| `discovery_timeout` | no | `30s` | How long to wait for an advertisement before giving up. |
| `connect_timeout` | no | `20s` | |
| `handshake_timeout` | no | `8s` | |
| `operation_timeout` | no | `120s` | Total budget for one request, across every retry. |
| `stuck_connect_timeout` | no | `60s` | See *Known limitations*. |
| `recover_by_reboot` | no | `false` | Reboot the node if a connection attempt is unrecoverably stuck. |
| `recover_after` | no | `5min` | How long to stay stuck before that reboot. |

## Top-down bottom-up (`motionblinds_ble_tdbu:`)

Two motors, one window, and no awareness of each other. Both supported
constructions are the same geometry — the rails delimit a segment, and only its
meaning differs:

```
  between_rails                      outside_in

  0 +--------------+                 0 +--------------+
    |              |  open             |##### fabric #|  COVERED
    |==== top =====|                   |==== top =====|
    |#### fabric ##|  COVERED          |              |  open
    |=== bottom ===|                   |=== bottom ===|
    |              |  open             |##### fabric #|  COVERED
100 +--------------+               100 +--------------+

  closed = fabric spans the window   closed = rails meet in the middle
```

| Variable | Required | Default | Description |
| --- | --- | --- | --- |
| `top_motor` / `bottom_motor` | yes | | The two motors. |
| `fabric` | no | `between_rails` | `between_rails` or `outside_in`. |
| `min_gap` | no | `5%` | How close the rails may physically come. Stacked fabric takes up room. |
| `safety_margin` | no | `2%` | Added on top when computing targets, for whole-numbered feedback and motor overshoot. |
| `clearance_timeout` | no | `60s` | How long the second rail waits for the first to make room. |
| `lease_timeout` | no | `180s` | Upper bound on holding both connections open. |

### Positions mean openness

`0%` is closed and `100%` is open, for every rail and both fabrics, and more
fabric across the window is always more closed.

This differs from the Home Assistant integration, which inherits the gateway's
convention where a raised top rail counts as "open" while it covers the most
window. If you prefer a rail the other way round, that is what `invert` on the
motor is for.

### Sliding the fabric is a separate entity

For `between_rails`, moving the block of fabric up and down without changing how
much of it shows is a different quantity from openness, and gets its own
`number`:

```yaml
number:
  - platform: motionblinds_ble_tdbu
    tdbu_id: living_blind
    fabric_position:
      name: "Living blind fabric position"
```

Folding both into one cover — as the Home Assistant integration does, where the
slider means "where the fabric is" while open and close mean "how much shows" —
lets a blind parked at the top and a blind covering the whole window report the
same position. The two are kept apart here, and the component merges them into
one desired geometry so that using one does not undo the other.

### Collision avoidance

Rails that touch jam the motors and stop mid-travel. Five rules, in order:

1. **Never move on a remembered position.** Both motors are asked where they
   are first. If that fails, the move is refused rather than guessed at. Only
   moves whose physical direction is fixed regardless of position — stop, and
   retreating a rail to its own far end — are exempt, which is what keeps a
   blind recoverable after a restart.
2. **The rail that opens the gap goes first.** The one closing it starts only
   once there is *observed* room. A completed write is not observation. Where
   both rails close in on each other, the second waits for the first to
   actually arrive, not merely to have started.
3. **Targets carry a margin.** `safety_margin` sits on top of `min_gap`,
   because position feedback is whole-numbered and these motors' overshoot is
   not specified anywhere.
4. **A failure stops the move.** If the first rail does not accept its command,
   or never makes room, the second rail is not moved at all.
5. **A watchdog runs throughout.** Every position frame is checked, and both
   rails are stopped the moment the observed gap drops below `min_gap`. This is
   what covers the moves the planning cannot see: a stalling rail, the physical
   remote, and the favorite button.

## Diagnostics and controls

```yaml
sensor:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    battery_level: {name: "Battery"}
    signal_strength: {name: "Signal"}

binary_sensor:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    position_fresh: {name: "Position fresh"}
    calibrated: {name: "Calibration"}

text_sensor:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    connection_status: {name: "Connection"}

select:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    speed: {name: "Speed"}

button:
  - platform: motionblinds_ble
    motionblinds_ble_id: living_top
    action: status_query
    name: "Refresh"
```

`connection_status` reports `disconnected`, `discovering`, `connecting`,
`connected`, `disconnecting` or `error`. Waiting on it is how an automation can
tell a real failure from a slow motor.

`position_fresh` reports whether the position was actually observed during the
current connection. The cover's own `assumed_state` flag cannot express this:
the native API only sends it when entities are listed, so it cannot change at
runtime.

`button` takes an `action` of `status_query`, `favorite`, `connect` or
`disconnect`. `status_query` connects and refreshes position, battery, speed
and calibration.

## Known limitations

- **A stuck connection attempt cannot be cancelled.** If the Bluetooth stack
  never reports the outcome of a connection attempt, there is no way to abort
  it through ESPHome's public API, and forcing the state locally would risk
  leaking a live link. The component reports the error and, with
  `recover_by_reboot`, restarts the node.
- **Moves made while disconnected are seen late.** Someone using the physical
  remote while nothing is connected is only noticed at the next connection.
  Staying connected permanently would fix this at the cost of keeping
  battery-powered motors' radios awake.
- **`stop`, `speed` and `favorite` cannot be verified.** The motor reports
  nothing that confirms them, so they are reported as sent, not as done.
- **The favorite position is unknowable.** The motor says whether one exists,
  never where it is, so the coordinator cannot check it against the other rail.
  On a top-down bottom-up blind the clearance watchdog is the only protection
  for that button.
- **Collision protection is at target level plus a margin.** Two independent
  motors, commanded separately, cannot offer an absolute guarantee.

## Development

The frame format and the collision geometry have no ESPHome or Bluetooth
dependencies, so both run as host tests:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -I esphome/components/motionblinds_ble \
  test/crypt_test.cpp \
  esphome/components/motionblinds_ble/motionblinds_aes.cpp \
  esphome/components/motionblinds_ble/motionblinds_crypt.cpp -o crypt_test && ./crypt_test

g++ -std=c++17 -O2 -Wall -Wextra -I . -I esphome/components/motionblinds_ble_tdbu \
  test/tdbu_test.cpp -o tdbu_test && ./tdbu_test
```

The frame tests run against golden vectors produced by the reference
`motionblindsble` Python library, so a pass means this implementation agrees
with the one known to drive these motors. The geometry test sweeps every
reachable pair of rail positions against every request a user can make, in both
fabrics, with inverted and partially calibrated rails, and asserts that no
command it would emit can bring the rails closer than `min_gap`.

What those suites do **not** cover is the connection and coordination state
machines, which need ESPHome and a motor to exercise. Their behaviour under
lost Bluetooth events, a stalling rail or a motor that accepts commands without
moving has been reasoned about and instrumented, but not yet observed on
hardware.

## Credits

The wire format was derived from [`motionblindsble`][lib] by @LennP, the library
behind the Home Assistant [Motionblinds Bluetooth][core] integration.

[lib]: https://github.com/LennP/motionblindsble
[core]: https://www.home-assistant.io/integrations/motionblinds_ble
