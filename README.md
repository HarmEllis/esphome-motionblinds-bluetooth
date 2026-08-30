# ESPHome Motionblinds Bluetooth

An [ESPHome](https://esphome.io) component that drives Motionblinds Bluetooth
blind motors directly from an ESP32 — including top-down bottom-up blinds, where
two motors share one window and must not be driven into each other.

No hub, no Home Assistant integration in the path, and no step that can wait
forever: every phase of every command has a deadline, and a command whose effect
cannot be established is reported as a failure rather than as a success.

```yaml
external_components:
  - source: github://HarmEllis/esphome-motionblinds-bluetooth@v0.0.28-beta2
    components: [motionblinds_ble, motionblinds_ble_tdbu]
```

## Contents

- [Quick start](#quick-start) — [single motor](#a-single-motor) · [top-down bottom-up](#a-top-down-bottom-up-blind)
- [Requirements and limits](#requirements-and-limits)
- [Configuration](#configuration) — [motor](#motor-motionblinds_ble) · [blind](#blind-motionblinds_ble_tdbu) · [entities](#entities)
- [What a position means](#what-a-position-means)
- [Collision avoidance](#collision-avoidance)
- [Making it fast](#making-it-fast)
- [What survives a restart](#what-survives-a-restart)
- [Running more than one node](#running-more-than-one-node)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Why this exists](#why-this-exists)
- [Development](#development) · [Field notes](#field-notes) · [Credits](#credits)

## Quick start

Every node needs these three blocks, whatever it drives:

```yaml
external_components:
  - source: github://HarmEllis/esphome-motionblinds-bluetooth@v0.0.28-beta2
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
    # Strongly recommended, and worth more than any other single setting.
    # See "Making it fast" for what it does and why the default hurts here.
    connection_scan_window: 160ms
```

### A single motor

```yaml
motionblinds_ble:
  # Motors are identified by the four characters they advertise as MOTION_XXXX,
  # which is what the vendor app and the sticker on the motor show.
  - id: office_blind
    mac_code: 0A5A
    time_id: ha_time
    blind_type: roller
    fast_connect: true

cover:
  - platform: motionblinds_ble
    motionblinds_ble_id: office_blind
    name: "Office blind"
```

That is the whole configuration. No `ble_client:` block — the Bluetooth client
is created for you.

### A top-down bottom-up blind

Two motors, one window. The `motionblinds_ble_tdbu:` block is what stops them
being driven into each other.

```yaml
motionblinds_ble:
  - id: living_top
    mac_code: 0A5A
    time_id: ha_time
    blind_type: honeycomb
    fast_connect: true
  - id: living_bottom
    mac_code: EBAA
    time_id: ha_time
    blind_type: honeycomb
    fast_connect: true
    invert: true          # this motor is mounted upside down

motionblinds_ble_tdbu:
  - id: living_blind
    top_motor: living_top
    bottom_motor: living_bottom
    fabric: between_rails
    optimistic: true      # see "Making it fast"
    diagnostics:
      name: "Living blind"

cover:
  - platform: motionblinds_ble_tdbu
    tdbu_id: living_blind
    rail: top
    name: "Living blind top"
  - platform: motionblinds_ble_tdbu
    tdbu_id: living_blind
    rail: bottom
    name: "Living blind bottom"
```

`rail: combined` gives you a single cover for how much of the window is
covered, instead of or alongside the two rail covers.

That one `diagnostics:` block adds twelve more entities — battery, signal,
connection status and position freshness for both rails, a refresh button for
each, a prepare button and a status text for the blind. See [entities](#entities).

> **Set `fast_connect: true` and, on a paired blind, `optimistic: true`.** They
> are off by default because they trade a little certainty for a lot of speed,
> and that is a choice to make deliberately rather than inherit. Without them a
> single nudge can take half a minute. [Making it fast](#making-it-fast)
> explains exactly what each one gives up.

## Requirements and limits

- **An ESP32 within Bluetooth range of the motors.** Range matters more than
  anything else here: motors are reached by waiting for their advertisements,
  and how long that takes is the largest part of the delay before a blind moves.
- **At most six motors per ESP32.** Bluetooth's `GATT_MAX_APPS` is 8 and keeps
  two for its own use, leaving six client registrations. The component rejects a
  seventh at compile time, because ESPHome itself only warns and the build would
  otherwise fail on the ESP during registration.
- **No `bluetooth_proxy` on a node with six motors**, and no hand-written
  `ble_client` either — same pool.
- `esp32_ble_tracker` must scan continuously; the component validates this.

## Configuration

### Motor (`motionblinds_ble:`)

One entry per motor. A top-down bottom-up blind has two.

| Option | Required | Default | Description |
| --- | --- | --- | --- |
| `id` | yes | | Identifier used by the platforms below. |
| `mac_code` | one of | | The four characters from the motor's `MOTION_XXXX` name, e.g. `0A5A`. The full address is learned from the first matching advertisement, so a motor that randomises its address still works. |
| `mac_address` | one of | | The motor's full address, if you would rather pin it. Exactly one of `mac_code` and `mac_address` is required. |
| `time_id` | yes | | A `time:` component. Commands carry a timestamp and are refused while the clock is unsynchronised. |
| `blind_type` | no | `roller` | `roller`, `honeycomb`, `roman`, `venetian`, `double_roller`, `curtain`, `vertical`. Curtain and vertical motors get a longer pause after keying. |
| `invert` | no | `false` | The motor is mounted upside down, so its positions run the other way through the window. |
| `window_min` / `window_max` | no | `0` / `100` | The part of the window this rail actually travels. Needed when the two motors of one blind are each calibrated over their own half. |
| `fast_connect` | no | `false` | Send waiting work as soon as the motor is keyed, instead of asking where the rail is first. See [Making it fast](#skip-the-status-query-when-there-is-already-work-to-do). |
| `low_latency_connection` | no | `true` | Prefer an 8.75–11.25 ms BLE connection interval during GATT setup and the handshake. Disable only if a motor proves incompatible. Independent of `fast_connect`. |
| `cached_connect` | no | `true` | Reuse the learned address and address type so the controller can wait directly for the motor's next advertisement. Stored across restarts when `high_duty_cycle_connect` is enabled; a failed shortcut automatically returns to normal discovery. |
| `high_duty_cycle_connect` | no | `true` | Use ESP-IDF's cancellable enhanced-open path and listen during 100% instead of 50% of the initiator scan interval. This raises ESP radio use only while establishing a connection, not the blind's connected time. |
| `disconnect_delay` | no | `15s` | Idle time before the connection is dropped; only starts once no move is in progress. Lower it on a node with several motors — see [Making it fast](#let-a-finished-motor-release-the-radio). |
| `discovery_timeout` | no | `30s` | Listening time per round. Counted only while the scanner is actually running, because the tracker stops scanning whenever any client is connecting. |
| `discovery_rounds` | no | `3` | Bounded listening rounds before giving up, with a growing pause between them. One window is fragile for a motor that advertises weakly; this is not an unbounded retry. |
| `connect_timeout` | no | `20s` | Link establishment and service discovery. |
| `handshake_timeout` | no | `15s` | Keying and the first status frame. Generous on purpose: several motors, Wi-Fi and logging share one radio, and a connection that is merely slow is not a failure. |
| `operation_timeout` | no | `180s` | Total budget for one request, across every retry. |
| `stuck_connect_timeout` | no | `60s` | When to report a connection attempt the Bluetooth stack never resolved. See [Known limitations](#known-limitations). |
| `recover_by_reboot` | no | `false` | Reboot the node if such an attempt stays stuck. |
| `recover_after` | no | `5min` | How long to stay stuck before that reboot. |

### Blind (`motionblinds_ble_tdbu:`)

One entry per top-down bottom-up blind. Both supported constructions are the
same geometry — the rails delimit a segment, and only its meaning differs:

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

| Option | Required | Default | Description |
| --- | --- | --- | --- |
| `id` | yes | | Identifier used by the `cover` and `number` platforms. |
| `top_motor` / `bottom_motor` | yes | | The two motors. |
| `fabric` | no | `between_rails` | `between_rails` or `outside_in`. |
| `optimistic` | no | `false` | Move from the remembered rail positions instead of reading both motors first. The single largest influence on how responsive the blind feels — see [Making it fast](#trust-what-is-remembered-optimistic). |
| `preconnect_trailing` | no | `true` | While the leading rail opens a safe gap, connect the rail that will move second. Its movement command remains blocked until clearance is observed. |
| `prepare_timeout` | no | `120s` | How long the diagnostics `prepare` button keeps both links warm for an upcoming command. |
| `min_gap` | no | `0%` | How close the rails may be commanded. Zero by default: on most of these blinds the rails stack against each other at either end, and the motors stop themselves if they meet. Raise it only for a blind whose rails genuinely cannot come together. |
| `safety_margin` | no | `0%` | Added on top of `min_gap` when computing targets, for whole-numbered feedback and motor overshoot. |
| `start_gap` | no | `10%` | Observed gap below which two rails may not be *set off* at the same moment. They may travel together; starting together while they are close is what drives them into each other. |
| `clearance_timeout` | no | `60s` | How long the second rail waits for the first to make room. |
| `lease_timeout` | no | `180s` | Upper bound on holding both connections open. |
| `diagnostics` | no | | Builds the per-rail diagnostic entities; see below. |

### Entities

**Covers.** For a paired blind, `motionblinds_ble_tdbu` with `rail: top`,
`bottom` or `combined`. For a lone motor, `motionblinds_ble` with
`motionblinds_ble_id`. Both accept the usual `cover` options.

**Diagnostics for a whole blind.** One block, twelve entities:

```yaml
motionblinds_ble_tdbu:
  - id: living_blind
    # ...
    diagnostics:
      name: "Living blind"
```

That gives a status text for the blind plus, for **both** rails: battery, signal
strength, connection status, position freshness and a refresh button — named
after the prefix (`Living blind status`, `Living blind top battery`,
`Living blind bottom refresh`, and so on). The coordinator already knows both
motors, so declaring twenty near-identical entities for a three-blind room is
busywork.

It also adds `Living blind prepare`. Press it shortly before a scheduled move
to connect and key both motors without moving either rail. The status changes
from `preparing both rails` to `ready for immediate command`; the next cover
command then reuses those links and the leases expire automatically after
`prepare_timeout`. In Home Assistant, call the button from an automation one or
two minutes before the cover action.

The status text is the one to watch when nothing moves: it names the rail that
blocked the move rather than leaving you to infer it.

**Diagnostics per motor**, when you want a different subset, different names, or
a motor that is not part of a paired blind:

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
    action: status_query          # status_query | favorite | connect | disconnect
    name: "Refresh"
```

**Fabric position**, for `between_rails` blinds — see
[Sliding the fabric](#sliding-the-fabric-is-a-separate-entity):

```yaml
number:
  - platform: motionblinds_ble_tdbu
    tdbu_id: living_blind
    fabric_position: {name: "Living blind fabric position"}
```

What the diagnostic entities mean:

| Entity | Meaning |
| --- | --- |
| `connection_status` | `disconnected`, `discovering`, `connecting`, `connected`, `disconnecting` or `error`. Waiting on this is how an automation tells a real failure from a slow motor. |
| `position_fresh` | Whether the position was actually observed during the current connection, as opposed to remembered. The cover's own `assumed_state` cannot express this — the native API only sends it when entities are listed, so it cannot change at runtime. |
| `calibrated` | Whether the motor reports both end positions. Stays empty until a motor has said something about them; an uncalibrated motor refuses position commands. |
| `battery_level` | Refreshed by every status frame, and remembered across a restart. |
| `signal_strength` | Deliberately not remembered: a stored RSSI describes a radio moment that has passed. Empty until the motor is next heard. |
| `speed` | `low`, `medium` or `high`. Not remembered — the motor has to say. |

Because `diagnostics:` builds these for you, the component auto-loads `sensor`,
`binary_sensor`, `text_sensor` and `button`, which costs a little flash even if
you never use it.

## What a position means

`0%` is closed and `100%` is open, as Home Assistant labels every cover. For a
**single rail** that is how high the rail is sitting:

| Rail | `0%` | `100%` |
| --- | --- | --- |
| top | lowered to the foot of its travel | raised to the head |
| bottom | down at the sill | raised to the head of its travel |

So the up arrow raises the rail and the down arrow lowers it, for both rails.
This is the convention Home Assistant's gateway-based `motion_blinds`
integration uses for the same blinds, so a top-down bottom-up shade behaves the
same way here as it does there. Note that the top rail raised is called "open"
even though that is where it covers the *most* window — the fabric hangs from
it. That reads oddly, but matching the established convention matters more than
fixing the wording.

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
`number` (see [entities](#entities)).

Folding both into one cover — as the Home Assistant integration does, where the
slider means "where the fabric is" while open and close mean "how much shows" —
lets a blind parked at the top and a blind covering the whole window report the
same position. The two are kept apart here, and the component merges them into
one desired geometry so that using one does not undo the other.

## Collision avoidance

These blinds stop themselves when the rails meet, and on most of them the rails
are meant to stack against each other at either end — a blind collapsed at the
head has both rails at the head. So the job is not to keep the rails apart; it is
to keep them from being driven into each other, and above all to start them in
the right order. Five rules:

1. **Move on an observed position.** Both motors are asked where they are before
   a guided move, and if that fails the move is refused rather than guessed at.
   Moves whose physical direction is fixed regardless of position — stop, and
   retreating a rail to its own far end — are exempt, which is what keeps a
   blind recoverable after a restart. **`optimistic: true` deliberately waives
   this rule**; the other four still hold.
2. **The rail that opens the gap goes first**, and the second starts only once
   the first has *observably* opened the gap past `start_gap`. A completed write
   is not observation. When the rails already have more than `start_gap` between
   them, both start together — they are allowed to travel at the same time; what
   they must not do is set off together while they are close.
3. **Targets never cross.** A rail is clamped so it cannot be commanded past the
   other one, plus `min_gap` and `safety_margin` where a blind needs them.
4. **A failure stops the move.** If the first rail does not accept its command,
   or never makes room, the second rail is not moved at all.
5. **A watchdog runs throughout.** Every position frame is checked and both
   rails are stopped the moment the observed gap drops below `min_gap`. At the
   default of zero that means only a genuine crossing. This covers what planning
   cannot see: a stalling rail, the physical remote, and the favorite button.

## Making it fast

A blind that takes half a minute to respond is technically working and
practically useless. Most of that time is not the rail moving — it is the radio
finding the motor and getting a link up.

Every connection ends with a line naming what each phase cost:

```
[living_top] Ready 5.0s after being wanted: heard 3.3s, link 1.1s,
services 0.0s, notifications 0.3s, key 0.2s
```

Each phase has a different remedy, which is the whole point of splitting them:
*heard* is the scanner's duty cycle, *link* is the controller reaching the motor,
*services* is GATT discovery and its cache, and *notifications* and *key* are the
motor answering. **Fix the phase you actually have.**

Every position write also reports the end-to-end queue time:

```
[living_top] Position command reached BLE 1.247s after it was requested
```

That is the number to compare while testing this pre-release. It includes
advertisement discovery, connection setup, keying and any safety wait, but not
the rail's mechanical reaction after the write.

### Prepare scheduled moves

No firmware can hear a battery motor before it advertises. For a move whose
time is known in advance, the only way to remove that cold-start wait from the
visible response is to pay it just beforehand. The TDBU diagnostics `prepare`
button opens both links, obtains fresh positions and holds them for two minutes
by default. It does not send a movement command.

Trigger it from Home Assistant roughly 60–90 seconds before the cover command.
Wait for the diagnostic status `ready for immediate command` when exact timing
matters. This works with both `fast_connect: false` and `true`: once both motors
are ready and their positions are fresh, neither path performs a redundant
status query.

For close rails that must move in sequence, `preconnect_trailing: true` overlaps
the second motor's connection with the first rail opening clearance. The second
movement is still withheld until position feedback proves the gap is safe.

### Reuse the learned BLE identity

The first request after a boot must hear an advertisement to learn both the
motor's full address and its address type. Older versions repeated that first
wait on every cold connection even though both values were still in RAM. Only
then did the BLE initiator start and wait for the motor's *next* advertisement.

`cached_connect: true` (the default) skips that redundant first wait after a
motor has been found once. It enters ESPHome's normal serialised connection
queue immediately, so several motors still connect one at a time; it does not
keep a link open and therefore adds no connected-radio time on the motor.

The learned pair is stored across restarts when `high_duty_cycle_connect` is
enabled. A random address can change, so a restored shortcut gets only twelve
seconds or the configured `connect_timeout`, whichever is shorter. If it does
not connect, enhanced open is cancelled, the stored pair is erased and the
existing retry returns to advertisement discovery. The first connection after
installing this version primes the flash cache; subsequent reboots can reuse it.

### Listen throughout link establishment

ESP-IDF's legacy BLE initiator listens for 30 ms in every 60 ms interval. A
motor advertisement landing in the other half can be missed, which costs a
whole extra advertising cycle. `high_duty_cycle_connect: true` uses the
enhanced-open API with a 40/40 ms scan window while the link is being
established. The extra receive time is on the mains-powered ESP and ends as soon
as the link opens; it does not keep the blind connected.

Enhanced open also supplies the missing cancellation primitive. An absent motor
is cancelled at `connect_timeout`, allowing the scanner and the other motors to
continue, with the longer `stuck_connect_timeout` retained only as a guard for a
Bluetooth stack that fails even to complete cancellation.

### Raise `connection_scan_window`

The single largest cost is usually waiting to hear a motor advertise, and on
ESPHome 2026.8 and later that wait is roughly ten times longer than it needs to
be, for a reason that is invisible in the config.

When `wifi:` is configured and ESP-IDF is at least 5.5.5, ESPHome raises a
defaulted scan window to the full interval — 320 ms out of 320 ms, a 100% duty
cycle — and at the same time injects `connection_scan_window: 30ms`, which takes
over **whenever any client is connected**. A general-purpose node wants that:
scanning at full duty while a connection is live starves both Wi-Fi and the
connection events. A node that exists only to drive these motors does not.

This component connects one rail, holds it for the length of the move, and has
to discover the other rail during exactly that window. Measured on hardware: a
rail went unheard for fifteen seconds while its partner was connected, then was
found 1.5 seconds after that partner disconnected.

```yaml
esp32_ble_tracker:
  scan_parameters:
    continuous: true
    connection_scan_window: 160ms
```

The value must not exceed the scan window, so 320 ms is the ceiling and disables
the fallback entirely. 160 ms is a reasonable first step rather than a measured
optimum. Raise it gradually and watch what it costs: lost status frames,
handshake retries, and API or Wi-Fi reconnects all show up before discovery
stops improving.

On ESPHome before 2026.8 this option does not exist and the window is 30 ms at
all times, connected or not.

### Trust what is remembered (`optimistic`)

By default nothing moves until **both** motors have reported their position
during the current connection. That is the honest position to take: these motors
can also be moved by their own remote or by the vendor app, and a remembered
position is then simply wrong.

It is also **slow**, and the cost is easy to underestimate. Nudging one rail
means waking both motors, and the Bluetooth stack connects to one device at a
time and stops scanning while it does, so the two connections happen one after
the other before anything moves at all. Twenty to sixty seconds for a single
rail is normal, and worse for a motor with a weak signal.

With `optimistic: true`, positions are taken from what the component last saw and
remembers across restarts. A move is planned and sent immediately, and only the
rails that actually move are woken.

> **Only set this if nothing else moves the blind.** If someone uses the physical
> remote, or the vendor app, while the node is not connected, the remembered
> positions become wrong and stay wrong until that rail is next commanded. The
> component will then plan a move — including which rail may start first — from a
> picture of the blind that does not match reality.
>
> Two things still hold either way: a rail is never commanded past the other one,
> and the clearance watchdog stops both rails if it sees them crossing while
> connected. What optimistic mode gives up is the guarantee that the picture was
> correct *before* the move started. Press a rail's refresh button, or command
> it, to resynchronise.

The option belongs to the blind rather than the motor because a single motor has
nothing to skip: it sends its position command as soon as its own connection is
up and never waits on a second motor.

### Skip the status query when there is already work to do

Normally a motor is not considered ready until it has been keyed *and* has
answered a status query — one full round trip before anything moves, and on a
weak link the place where a lost write costs a three-second retry.

`fast_connect: true` sends the waiting command as soon as the motor is keyed.

Nothing is reported as done that was not: the command still has to be confirmed
by the motor before it counts, so a blind that did not move is still a failure.
What is given up is finding out *early* that the key never landed; that case then
surfaces as a travel timeout rather than a handshake timeout — several seconds
later, and named less precisely.

Two things it deliberately does not do. A position command is held back when no
position is remembered at all, because the travel budget is derived from the
distance to cover and a budget measured from a position that was never observed
can cut a legitimate move short. And it changes nothing when the motor has no
work waiting, such as a refresh button press.

One consequence is handled for you. Battery, speed and favourite arrive only in a
status frame; the feedback frames a move produces do not carry them. So when the
work is done and those values are over an hour old, the query is sent then,
before the link is dropped, using time the motor was going to spend idle. It is
postponed until the normal idle deadline so a real follow-up command gets the
next command slot instead of waiting behind this background refresh; after its
answer the link is dropped immediately rather than paying the idle delay twice.

### Let a finished motor release the radio

`disconnect_delay` defaults to 15 seconds. On a node with one blind that costs
nothing. On a node with three, a motor that finished its move keeps its link
alive for fifteen seconds while the scanner — now at the reduced window above —
is trying to hear the next one.

```yaml
motionblinds_ble:
  - id: living_bottom
    disconnect_delay: 3s
```

What it costs: a genuine follow-up command arriving inside the old window now
pays for a fresh connection instead of reusing the open one. If you routinely
nudge the same blind several times in a row, keep it higher.

### What does not help

Raising `disconnect_delay` so links stay warm, keeping connections open
permanently, and tuning the MTU have all been considered and rejected; the
reasoning is in the [field notes](docs/field-notes.md). Lowering the minimum gap
between commands is actively harmful — it is the guard against the fault where a
second command sent too soon is accepted and silently ignored.

`link` time is not explained by signal strength: 1.1 s at −78 dBm on one motor,
10.2 s at −60 dBm on another. What `esp_ble_gattc_open()` waits for is the
target's next advertisement, so the number that matters is how often that
particular motor advertises. Cached connect cannot change that interval; it can
start waiting one advertisement earlier.

The honest ceiling: connections on one ESP32 are established one at a time, and
that is the tracker's design, not a bug. Cached connect shortens each path but
does not make them parallel. If the remaining sum is not fast enough, the answer
is a second node.

## What survives a restart

Each motor keeps its last known position, tilt and battery reading in flash, so a
blind comes back knowing roughly where its rails are instead of blank. Written
when a move completes and when the link is dropped — a handful of writes a day,
not thousands, because flash wear is real.

A restored position is deliberately **not** treated as fresh. Only a frame from
the current connection can justify a movement decision, because a remote or the
vendor app can move a rail while the node is powered off. `position_fresh` is
what tells the two apart.

At startup each motor reports which of three things happened, at `INFO` level:

```
[living_top] Restored position 50, battery 89%
[living_top] Flash store held nothing; this rail does not know where it is
[living_top] Flash store was read but holds no position; ...
```

Two cases lose the stored state on purpose, and a single refresh restores it:

- **The stored format changed.** The blob carries a version; a blob written by a
  different version is discarded rather than reinterpreted.
- **A motor's identifier changed.** The storage key is derived from whichever of
  `mac_code` and `mac_address` the motor is configured with, and the full address
  cannot be derived from the four-character code at build time, so the two cannot
  be made to agree.

## Running more than one node

Six motors is the ceiling on one ESP32, but it is not always the right number. A
motor at the far end of the house shares a radio with five others and drags out
every group operation; a second node next to it may beat any amount of tuning.

Split by blind, never by rail: both motors of one top-down bottom-up blind must
be on the same node, because the coordinator that keeps them apart lives there.

Set each node's `esp32_ble: max_connections:` to the number of motors it actually
has. And **remove a motor from the old node before the new one goes live** — two
nodes fighting over one motor is exactly the fault this component was written to
avoid, and nothing in either config can detect it.

## Troubleshooting

**Nothing moves, and the blind's status text says why.** That entity names the
rail that blocked a move. Start there rather than in the log.

**A cover reads 100% and you do not believe it.** A cover position is a plain
number with no room for "I do not know", and a cover the component has never
published to reads as fully open. `position_fresh` and the blind's status text
are the authoritative signals — the cover is not.

**A motor connects and then does nothing.** These motors sometimes accept a
connection, confirm their notifications and accept the key, and then say nothing:
no status frame, so no battery, no speed, and no movement from any command sent
over that link. It is not a signal problem — it has been seen on a −63 dBm
connection — and it is a known, still-open fault of the Home Assistant
integration this replaces
([core#153218](https://github.com/home-assistant/core/issues/153218)), where it
shows up as a blind stuck at "Connected" with battery and speed `unknown`.

Asking again over the same link does not help; dropping the connection and making
a fresh one does, usually after two to four attempts. This component does that
itself, so it appears in the log as a retry rather than as an error.

**Commands in quick succession get ignored.** A second command sent too soon is
accepted and silently dropped by the motor. The component enforces a minimum gap
between commands for this reason. `stop` is exempt — a brake that waits its turn
is not a brake.

**Boot-time log lines never appear.** Anything ESPHome logs during `setup()` is
emitted before the API is up, so neither Home Assistant nor the ESPHome dashboard
log ever sees it. Only a serial cable shows those. This component reports its
startup state from `dump_config()` instead, which is late enough to be readable.

## Known limitations

- **The Bluetooth stack can still fail to complete cancellation.** Enhanced
  open normally lets the component cancel an absent motor at `connect_timeout`.
  If the stack does not report even that outcome, `stuck_connect_timeout` and
  optional `recover_by_reboot` remain the final guard.
- **Moves made while disconnected are seen late.** Someone using the physical
  remote while nothing is connected is only noticed at the next connection.
  Staying connected permanently would fix this at the cost of keeping
  battery-powered motors' radios awake.
- **`stop`, `speed` and `favorite` cannot be verified.** The motor reports
  nothing that confirms them, so they are reported as sent, not as done.
- **The favorite position is unknowable.** The motor says whether one exists,
  never where it is, so the coordinator cannot check it against the other rail.
  On a paired blind the clearance watchdog is the only protection for that button.
- **Collision protection is at target level plus a margin.** Two independent
  motors, commanded separately, cannot offer an absolute guarantee.

## Why this exists

It grew out of living with the Home Assistant [`motionblinds_ble`][core]
integration across six motors and finding four separate ways for it to fail, none
of which announce themselves:

1. **A command can hang forever.** `cover.set_cover_position` has been observed
   still running after forty minutes, with no exception, no timeout and nothing
   in the log. The Bluetooth connection was healthy the whole time.
2. **A motor can become permanently unreachable** until its config entry is
   reloaded. Every call then fails instantly with a `BleakError` raised from a
   dead connection task, without the library ever trying to reconnect.
3. **A motor can accept commands and not move.** It connects, reports success,
   and sends no status frame at all — so the blind sits still while everything
   upstream believes it moved.
4. **A shared script pool can fill with hung runs**, after which nothing happens
   at all and no error appears anywhere. The automation traces look perfect; the
   failure is one layer below them.

Underneath all four sits the same root problem: **there is no deadline anywhere in
the connection or command path**, so a step that never completes simply never
completes.

Two further gaps this closes:

- **No top-down bottom-up support.** The integration configures one motor per
  config entry, so a paired blind appears as two unrelated blinds. Neither motor
  knows the other exists, and nothing stops one rail from being driven into the
  other. Working around that in automations means keeping positions in helper
  entities, which drift from reality the moment a command fails or someone picks
  up the remote.
- **Position and battery read `unknown` most of the time.** Both are only known
  while a connection is up, and the connection is dropped seconds after each
  command.

What this component does differently:

- **Nothing waits forever.** Discovery, connecting, the handshake, each command
  and the operation as a whole all have deadlines. Every failure path ends at a
  visible error state, never at silence.
- **A write is not a result.** The command characteristic is written without a
  response, so a successful write proves nothing about the motor. Position
  commands are followed until the motor reports it arrived; the commands that
  cannot be verified are named above rather than assumed to have worked.
- **A remembered position is never silently trusted.** It survives a restart so
  the entities are not blank, but it is marked stale, and `optimistic` is the
  explicit, documented choice to act on it anyway.
- **The connection is held for the whole move.** A rail keeps travelling after
  its command is sent. Disconnecting then would end the position updates the
  collision watchdog depends on, so both motors are pinned until the blind has
  come to rest.

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
`motionblindsble` Python library, so a pass means this implementation agrees with
the one known to drive these motors. The geometry test sweeps every reachable
pair of rail positions against every request a user can make, in both fabrics,
with inverted and partially calibrated rails, and asserts that no command it
would emit can bring the rails closer than `min_gap`.

CI additionally validates every fixture under `fixtures/` against a pinned
minimum ESPHome version and the current release, checks that the deliberately
invalid ones are rejected, and compiles the six-motor firmware.

Two of those fixtures — `quickstart_single.yaml` and `quickstart_tdbu.yaml` —
are the quick start from this file, so a documented example that stops being a
working configuration fails the build rather than wasting someone's evening.

What the host suites do **not** cover is the connection and coordination state
machines, which need ESPHome and a motor to exercise. Those have been run on
hardware — six motors across two nodes — but they are not covered by an automated
test, so treat changes there as needing a real blind.

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
