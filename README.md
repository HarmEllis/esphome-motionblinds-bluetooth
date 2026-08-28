# ESPHome Motionblinds Bluetooth

An [ESPHome](https://esphome.io) component that drives Motionblinds Bluetooth
motors directly from an ESP32, including top-down bottom-up blinds where two
motors share one window and must not be driven into each other.

## Why this exists

It grew out of living with the Home Assistant [`motionblinds_ble`][core]
integration across six motors and finding four separate ways for it to fail,
none of which announce themselves:

1. **A command can hang forever.** `cover.set_cover_position` has been observed
   still running after forty minutes, with no exception, no timeout and nothing
   in the log. The Bluetooth connection was healthy the whole time.
2. **A motor can become permanently unreachable** until its config entry is
   reloaded. Every call then fails instantly with a `BleakError` raised from a
   dead connection task, without the library ever trying to reconnect.
3. **A motor can accept commands and not move.** It connects, reports success,
   and sends no status frame at all — so the blind sits still while everything
   upstream believes it moved.
4. **A shared script pool can fill with hung runs**, after which nothing
   happens at all and no error appears anywhere. The automation traces look
   perfect; the failure is one layer below them.

Underneath all four sits the same root problem: **there is no deadline anywhere
in the connection or command path**, so a step that never completes simply
never completes. Every step here has one, and a command whose effect cannot be
established is reported as a failure rather than as a success.

Two further gaps this closes:

- **No top-down bottom-up support.** The integration configures one motor per
  config entry, so a top-down bottom-up blind appears as two unrelated blinds.
  Neither motor knows the other exists, and nothing stops one rail from being
  driven into the other. Working around that in automations means keeping the
  positions in helper entities, which drift from reality the moment a command
  fails or someone picks up the remote.
- **Position and battery read `unknown` most of the time.** Both are only known
  while a Bluetooth connection is up, and the connection is dropped seconds
  after each command. Here the last known position is kept across reboots, and
  battery arrives with every status frame — with an explicit
  `position_fresh` sensor so you can tell a remembered value from an observed
  one instead of guessing.

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
  # Motors are identified by the four-character code they advertise as
  # MOTION_XXXX, which is what the vendor app and the sticker on the motor
  # show. Use mac_address instead if you prefer to pin the full address.
  - id: living_top
    mac_code: 0A5A
    time_id: ha_time
    blind_type: honeycomb
  - id: living_bottom
    mac_code: EBAA
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
| `mac_code` | one of | | The four characters from the motor's `MOTION_XXXX` name, e.g. `0A5A`. The full address is learned from the first matching advertisement, which also means a motor that randomises its address still works. |
| `mac_address` | one of | | The motor's full address, if you would rather pin it. Exactly one of `mac_code` and `mac_address` is required. The Bluetooth client is created for you either way; there is no `ble_client:` block to write. |
| `time_id` | yes | | A `time:` component. Commands carry a timestamp and are refused while the clock is unsynchronised. |
| `blind_type` | no | `roller` | `roller`, `honeycomb`, `roman`, `venetian`, `double_roller`, `curtain`, `vertical`. Curtain and vertical motors get a longer pause after keying. |
| `invert` | no | `false` | The motor is mounted upside down, so its positions run the other way through the window. |
| `window_min` / `window_max` | no | `0` / `100` | The part of the window this rail actually travels. Needed when the two motors of one blind are each calibrated over their own half. |
| `disconnect_delay` | no | `15s` | Idle time before the connection is dropped. Only starts once no move is in progress. |
| `discovery_timeout` | no | `30s` | Listening time per round. Counted only while the scanner is actually running, because the tracker stops scanning whenever any client is connecting. |
| `connect_timeout` | no | `20s` | |
| `handshake_timeout` | no | `8s` | |
| `operation_timeout` | no | `120s` | Total budget for one request, across every retry. |
| `discovery_rounds` | no | `3` | Bounded listening rounds before giving up, with a growing pause between them. One window is fragile for a motor that advertises weakly; this is not an unbounded retry. |
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
| `diagnostics` | no | | Builds the per-rail diagnostic entities; see below. |

### Diagnostics come with the blind

```yaml
motionblinds_ble_tdbu:
  - id: living_blind
    top_motor: living_top
    bottom_motor: living_bottom
    diagnostics:
      name: "Living blind"
```

That one block creates a status text for the blind plus, for **both** rails:
battery, signal strength, connection status, position freshness, and a refresh
button — eleven entities named after the prefix (`Living blind status`,
`Living blind top battery`, `Living blind bottom refresh`, and so on).

The status text is the one to watch when nothing moves: it names the rail that
blocked the move, rather than leaving you to infer it.

The coordinator already knows both motors, so making you declare twenty
near-identical entities for a three-blind window is busywork. The individual
`motionblinds_ble` platforms below remain available when you want a different
subset, different names, or diagnostics for a motor that is not part of a
top-down bottom-up blind.

Because of this the component auto-loads `sensor`, `binary_sensor`,
`text_sensor` and `button`, which costs a little flash even if you never use
`diagnostics:`.

### What a position means

`0%` is closed and `100%` is open throughout. For the **combined** cover that is
coverage of the window as a whole. For a **single rail** it addresses the window
directly:

| Rail | `0%` | `100%` |
| --- | --- | --- |
| top | raised to the head | lowered to the foot of its travel |
| bottom | down at the sill | raised to the head of its travel |

These are the same two axes a top-down bottom-up blind card draws, so a preset
written for such a card means the same thing here.

> **The up arrow on a single rail is not "up".** Home Assistant labels position
> 100 "open" and draws an up arrow for it. For the top rail, open *is* the
> lowered end — the fabric hangs from that rail, so lowering it uncovers the
> window. Pressing open on the device page therefore sends the top rail down.
> That is not a bug and it is the only convention that keeps a blind card's
> presets meaning what they say, but it reads backwards on Home Assistant's own
> cover controls. Drive single rails from a blind card, or from the position
> slider, rather than the arrows.

A rail's position is scaled against **its own** travel, not against whatever the
other rail leaves free. Scaling against the other rail makes a stationary rail's
reading move whenever the other one travels, and makes a requested 50% land in a
different place depending on where the other rail happens to be.

Targets are still clamped so the rails keep their distance, and the cover then
reports the position actually reached — a clamped request reads back as the
position the rail got to, never as a end stop it never touched.

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

## Diagnostics and controls, per motor

For a single motor, or when you want to pick and name the entities yourself:

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
