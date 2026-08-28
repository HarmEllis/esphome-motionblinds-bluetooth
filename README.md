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
    # ESPHome 2026.8 and later scan at a full duty cycle while idle, but fall
    # back to a 30 ms window (9.4 %) as soon as any client is connected. That
    # is exactly when this component has to find the second rail, so raise it
    # on a node dedicated to these motors. Must not exceed the scan window.
    connection_scan_window: 160ms

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
| `disconnect_delay` | no | `15s` | Idle time before the connection is dropped. Only starts once no move is in progress. On a node with several motors, consider lowering it: a finished motor holding its link for fifteen seconds is competing for the radio precisely while the others are trying to be heard. |
| `discovery_timeout` | no | `30s` | Listening time per round. Counted only while the scanner is actually running, because the tracker stops scanning whenever any client is connecting. |
| `connect_timeout` | no | `20s` | |
| `handshake_timeout` | no | `15s` | Keying and the first status frame. Generous on purpose: six motors, Wi-Fi and logging share one radio, and a connection that is merely slow is not a failure. |
| `operation_timeout` | no | `120s` | Total budget for one request, across every retry. |
| `fast_connect` | no | `false` | Send waiting work as soon as the motor is keyed, instead of asking where the rail is first. Saves a round trip on every command; see *Making it fast*. |
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
| `min_gap` | no | `0%` | How close the rails may be commanded. Zero by default: on most of these blinds the rails stack against each other at either end, and the motors stop themselves if they meet. Raise it only for a blind whose rails genuinely cannot come together. |
| `safety_margin` | no | `0%` | Added on top of `min_gap` when computing targets, for whole-numbered feedback and motor overshoot. |
| `start_gap` | no | `10%` | Observed gap below which two rails may not be *set off* at the same moment. They may travel together; starting together while they are close is what drives them into each other. |
| `optimistic` | no | `false` | Trust the remembered rail positions instead of reading both motors before every move. See below — this is the single largest influence on how responsive the blind feels. |

### `optimistic`: responsiveness versus knowing where the rails are

By default (`optimistic: false`) nothing moves until **both** motors have
reported their position during the current connection. That is the honest
position to take: these motors can also be moved by their own remote or by the
vendor app, and a remembered position is then simply wrong.

It is also **slow**, and the cost is easy to underestimate. Nudging one rail
means waking both motors, and the Bluetooth stack connects to one device at a
time and stops scanning while it does, so the two connections happen one after
the other before anything moves at all. Twenty to sixty seconds for a single
rail is normal, and worse for a motor with a weak signal.

With `optimistic: true`, positions are taken from what the component last saw
and remembers across reboots. A move is planned and sent immediately, only the
rails that actually move are woken, and positions are still refreshed on every
connection — each one begins with a status query, and further frames arrive
while the blind travels.

> **Only set this if nothing else moves the blind.** If someone uses the
> physical remote, or the vendor app, while the node is not connected, the
> remembered positions become wrong and stay wrong until that rail is next
> commanded. The component will then plan a move — including which rail may
> start first — from a picture of the blind that does not match reality.
>
> Two things still hold either way: a rail is never commanded past the other
> one, and the clearance watchdog stops both rails if it sees them crossing
> while connected. What optimistic mode gives up is the guarantee that the
> picture was correct *before* the move started. Press a rail's refresh button,
> or command it, to resynchronise.

The option belongs to the blind rather than the motor because a single motor
has nothing to skip. A `cover: - platform: motionblinds_ble` sends its position
command as soon as its own connection is up; it never waits on a second motor,
so it is already as responsive as optimistic mode makes a paired blind. The wait
exists only in the coordinator, which needs both rail positions before it can
decide what may move and in which order.
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

The component also turns on ESP-IDF's `CONFIG_BT_GATTC_CACHE_NVS_FLASH`, so a
motor's discovered attribute table is kept in flash. Without it every
connection re-walks the whole table before a command can be sent, and on
battery devices with a slow connection interval that walk is a large part of
the delay between pressing a button and the blind moving.

### What a position means

`0%` is closed and `100%` is open, as Home Assistant labels every cover. For a
**single rail** that means how high the rail is sitting:

| Rail | `0%` | `100%` |
| --- | --- | --- |
| top | lowered to the foot of its travel | raised to the head |
| bottom | down at the sill | raised to the head of its travel |

So the up arrow raises the rail and the down arrow lowers it, for both rails.
This is the convention Home Assistant's gateway-based `motion_blinds`
integration uses for the same blinds, so a top-down bottom-up shade behaves the
same way here as it does there. Note that the top rail raised is called "open"
even though that is where it covers the *most* window — the fabric hangs from
it. That reads oddly, but it is the established convention and matching it
matters more than fixing the wording.

The **combined** cover means coverage of the window as a whole: `0%` fabric
across the whole window, `100%` fabric collapsed out of the way.

**One deliberate difference from the gateway integration.** There, a rail's
position is scaled against the travel the *other* rail leaves free, so a
stationary rail's reported position changes whenever the other one moves, and a
requested 50% lands somewhere different depending on where the other rail
happens to be. Here each rail is scaled against its own travel, so a position
means the same thing every time. Targets are still clamped to keep the rails
apart, and the cover reports the position actually reached — a clamped request
reads back as where the rail got to, never as an end stop it never touched.

> **Using a blind card?** Cards for these shades usually draw the top rail's
> axis as *how far it has come down*, which is the mirror of the position here.
> Set the card's `invert_top` option for that rail. `tdbu-blind-card` calls it
> exactly that.

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

These blinds stop themselves when the rails meet, and on most of them the rails
are meant to stack against each other at either end — a blind collapsed at the
head has both rails at the head. So the job here is not to keep the rails apart;
it is to keep them from being driven into each other in the first place, and
above all to start them in the right order. Five rules:

1. **Never move on a remembered position.** Both motors are asked where they
   are first. If that fails, the move is refused rather than guessed at. Only
   moves whose physical direction is fixed regardless of position — stop, and
   retreating a rail to its own far end — are exempt, which is what keeps a
   blind recoverable after a restart.
2. **The rail that opens the gap goes first**, and the second one starts only
   once the first has *observably* opened the gap past `start_gap`. A completed
   write is not observation. When the rails already have more than `start_gap`
   between them, both start together — they are allowed to travel at the same
   time; what they must not do is set off together while they are close.
3. **Targets never cross.** A rail is clamped so it cannot be commanded past
   the other one, plus `min_gap` and `safety_margin` where a blind needs them.
4. **A failure stops the move.** If the first rail does not accept its command,
   or never makes room, the second rail is not moved at all.
5. **A watchdog runs throughout.** Every position frame is checked, and both
   rails are stopped the moment the observed gap drops below `min_gap`. At the
   default of zero that means only a genuine crossing. This is what covers the
   moves the planning cannot see: a stalling rail, the physical remote, and the
   favorite button.

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

A rail whose position is not known at all — a motor that has never reported, on
a node with nothing stored in flash — has nothing honest to publish. A cover
position is a plain number with no room for "I do not know", and the value the
entity carries until something is written to it reads as **fully open**. Treat
`position_fresh` and the blind's status text as the authoritative signals, not
the cover, until a rail has been heard from once.

Positions **and battery readings** are committed to flash as soon as they change,
so this window is normally only a node's very first boot. Signal strength is
deliberately not kept: a remembered RSSI describes a radio moment that has
passed and says nothing about now, so it stays empty until the motor is next
heard.

`position_fresh` reports whether the position was actually observed during the
current connection. The cover's own `assumed_state` flag cannot express this:
the native API only sends it when entities are listed, so it cannot change at
runtime.

`button` takes an `action` of `status_query`, `favorite`, `connect` or
`disconnect`. `status_query` connects and refreshes position, battery, speed
and calibration.

## Making it fast

A blind that takes half a minute to respond is technically working and
practically useless. Most of that time is not the rail moving — it is the radio
finding the motor and getting a link up. What follows is what actually helps,
in the order worth trying, and one measurement that tells you whether it did.

Start by turning on the phase log. Every connection ends with a line like:

```
[tv_bottom] Ready 9.0s after being wanted: heard 0.6s, link 5.4s, services 0.3s,
notifications 0.4s, key 2.3s
```

Each phase has a different remedy, which is the whole point of splitting them:
*heard* is the scanner's duty cycle, *link* is the controller reaching the
motor, *services* is GATT discovery and its cache, and *notifications* and *key*
are the motor answering. Fix the phase you actually have.

### The scan window collapses while a motor is connected

The single largest cost in a group operation is not travel, it is waiting to
hear a motor advertise. On ESPHome 2026.8 and later that wait is roughly ten
times longer than it needs to be, for a reason that is invisible in the config.

When `wifi:` is configured and ESP-IDF is at least 5.5.5, ESPHome raises a
defaulted scan window to the full interval — 320 ms out of 320 ms, a 100 % duty
cycle — and at the same time injects `connection_scan_window: 30ms`, which
takes over whenever any client is connected. A general-purpose node wants that:
scanning at full duty while a connection is live starves both Wi-Fi and the
connection events. A node that exists only to drive these motors does not.

This component connects one rail, holds it for the length of the move, and has
to discover the other rail during that window. Measured on hardware: a rail went
unheard for fifteen seconds while its partner was connected, then was found
1.5 seconds after that partner disconnected.

Set it explicitly:

```yaml
esp32_ble_tracker:
  scan_parameters:
    continuous: true
    connection_scan_window: 160ms
```

The value has to be no larger than the scan window, so 320 ms is the ceiling and
disables the fallback entirely. 160 ms is a reasonable first step rather than a
measured optimum. Raise it gradually and watch what it costs: lost status
frames, handshake retries, and API or Wi-Fi reconnects all show up before
discovery stops improving.

On ESPHome before 2026.8 this option does not exist and the window is 30 ms at
all times, connected or not.

### A finished motor should let go of the radio

`disconnect_delay` defaults to 15 seconds. On a node with one blind that costs
nothing. On a node with three, a motor that finished its move keeps its link
alive for fifteen seconds while the scanner — now at the reduced window above —
is trying to hear the next one.

```yaml
motionblinds_ble:
  - id: tv_bottom
    disconnect_delay: 3s
```

What it costs: a genuine follow-up command that arrives inside the old window
now pays for a fresh connection instead of reusing the open one. If you
routinely nudge the same blind a few times in a row, keep it higher.

### Skip the status query when there is already work to do

Normally a motor is not considered ready until it has been keyed *and* has
answered a status query. Only then is the queued command sent. That is one full
round trip to the motor before anything moves, and on a weak link it is where a
lost write costs a three-second retry.

`fast_connect: true` sends the waiting command as soon as the motor is keyed:

```yaml
motionblinds_ble:
  - id: tv_bottom
    fast_connect: true
```

Nothing is reported as done that was not: the command still has to be confirmed
by the motor before it counts, so a blind that did not move is still a failure.
What is given up is finding out *early* that the key never landed. That case
then surfaces as a travel timeout rather than a handshake timeout — several
seconds later, and named less precisely.

Two things it deliberately does not do. A position command is held back when no
position is remembered at all, because the travel budget is derived from the
distance to cover and a budget measured from a position that was never observed
can cut a legitimate move short. And it changes nothing when the motor has no
work waiting, such as a refresh button press.

One consequence needs handling and is handled for you. Battery, speed and
favourite arrive only in a status frame; the feedback frames a move produces do
not carry them. A session that skipped the status query and then only moved
would leave those blank — the exact symptom this component exists to avoid. So
when the work is done and the values are older than an hour, the query is sent
then, before the link is dropped, using time the motor was going to spend idle.
Hourly rather than every time, because a held link is what makes the next motor
slow to find.

It pairs naturally with `optimistic: true` on the blind — both are the same
trade, made at different levels: act on what is remembered rather than pay to
re-establish it first.

### What does not help

Raising `disconnect_delay` so links stay warm, keeping connections open
permanently, and tuning the MTU have all been considered and rejected; the
reasoning is in the field notes. Lowering the minimum gap between commands is
actively harmful — it is the guard against the fault where a second command
sent too soon is accepted and silently ignored.

The honest ceiling: connections on one ESP32 are established one at a time, and
that is the tracker's design, not a bug. Three blinds moving from cold will not
beat roughly the sum of three connection paths. If that is not fast enough, the
answer is a second node, not a setting.

## What is remembered across a restart

Each motor keeps its last known position, tilt and battery reading in flash, so
a blind comes back knowing roughly where its rails are instead of blank. A
restored position is deliberately *not* treated as fresh: only a frame from the
current connection can justify a movement decision, because a remote or the
vendor app can move a rail while the node is powered off.

Two things are worth knowing.

The stored blob carries a format version. When that version changes, the old
blob is discarded rather than reinterpreted, and every motor comes back not
knowing where it is until it is next asked. Refreshing the blinds once restores
them.

The storage key is derived from whichever identifier the motor is configured
with. Switching a motor between `mac_code` and `mac_address` changes that
identity, so its stored state is lost once for the same reason. The full address
cannot be derived from the four-character code at build time, so the two cannot
be made to agree.

## Splitting motors across two nodes

Six motors is the ceiling on one ESP32, but it is not always the right number.
A motor at the far end of the house shares a radio with five others and drags
out every group operation; a second node next to it may beat any amount of
tuning, even on a board with a worse antenna, because proximity usually wins.

To split, move a whole blind — both its rails and its `motionblinds_ble_tdbu:`
entry — to the second node's configuration, and set each node's
`esp32_ble: max_connections:` to the number of motors it actually has.

> **A motor must appear on exactly one node.** Nothing can detect otherwise:
> each node validates only its own configuration, and neither has any way to
> know the other exists. Two nodes connecting to the same motor is the failure
> this component was built to escape — it is what an unremoved Home Assistant
> integration does, and the symptom is a motor that connects, accepts commands
> and does nothing.

Both rails of a blind must stay together, since the coordinator has to reach
both to keep them from colliding.

## The silent-after-keying connection

These motors sometimes accept a connection, confirm their notifications and
accept the key, and then say nothing: no status frame, so no battery, no speed,
and no movement from any command sent over that link. It is not a signal
problem — it has been seen here on a -63 dBm connection — and it is a known,
still-open fault of the Home Assistant integration this component replaces
([core#153218](https://github.com/home-assistant/core/issues/153218)), where it
shows up as a blind stuck at "Connected" with battery and speed `unknown`.

Asking again over the same link does not help. What works, and what people
using that integration have resorted to automating by hand, is dropping the
connection and making a fresh one — usually two to four attempts before one
takes. This component does that itself: a connection that goes quiet after
being keyed is retried like a dropped link rather than reported as the motor's
answer, within the same attempt limit as any other failure.

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

## Field notes

[`docs/field-notes.md`](docs/field-notes.md) records what these motors and
ESPHome's Bluetooth stack actually do, as opposed to what the code implies —
including the quirks that explain why parts of this component are as careful as
they are, the measurements behind the defaults, and the approaches that were
tried and rejected. Read it before changing anything in the connection or
collision paths.

## Credits

The wire format was derived from [`motionblindsble`][lib] by @LennP, the library
behind the Home Assistant [Motionblinds Bluetooth][core] integration.

[lib]: https://github.com/LennP/motionblindsble
[core]: https://www.home-assistant.io/integrations/motionblinds_ble
