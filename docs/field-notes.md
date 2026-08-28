# Field notes

Things that cost hours to find out and are not evident from the code. Most of
them are the reason some piece of this component looks more careful than it
needs to. Each entry says whether it was verified, and how.

Written while bringing six motors up on one ESP32-S3 in August 2026.

---

## The motors

### A connection can be alive, keyed, and completely inert

A motor accepts the connection, confirms its notification descriptor, accepts
the key — and then never sends a status frame. Battery and speed stay unknown,
and nothing commanded over that link has any effect.

Not a signal problem: seen here on a **-63 dBm** connection, the best reading
that motor produced all day.

It is a known, still-open fault of the Home Assistant integration
([core#153218](https://github.com/home-assistant/core/issues/153218)), where it
appears as a blind stuck at "Connected" with battery and speed `unknown`.
Reporters there converged on the same remedy independently: not patience, but a
**fresh connection**, typically two to four before one takes. One of them
automated exactly that.

Hence: a connection that goes silent after keying is retried like a dropped
link, not reported as the motor's answer.

### Writes are unacknowledged, and they do get lost

Every command is written without a response — that is what the protocol
expects, and asking for a response provokes an ATT "unlikely error" on these
motors. So a lost write is completely silent.

**Verified, 2026-08-28.** A handshake that had been failing repeatedly:

```
16:14:35.6  found at -64 dBm
16:14:48.3  No status yet, asking again (attempt 2 of 2)
16:14:48.5  Ready, position 0, battery 50%
```

Two hundred milliseconds after the repeat. The earlier "handshake timed out"
failures at 13–18 seconds were very likely all lost writes.

The library's own maintainer proposes the same remedy for this fault
([LennP/motionblindsble#5](https://github.com/LennP/motionblindsble/pull/5),
against core#153218): **three attempts, three seconds apart**, repeating only
the status query — not the key. Those are the numbers used here.

Related settings in that library, for reference: `SETTING_CONNECTION_DELAY` is
0.2 s and `SETTING_NOTIFICATION_DELAY` is 0.5 s. There is no two-millisecond
delay anywhere, despite the recollection that started this search.

### A motor at its target says nothing

Ask a rail to go where it already is and no frame comes back — there is nothing
to report. Waiting for an arrival that was never going to be announced times out
and looks exactly like a failure. Such a command is sent but not waited on.

### Commands in quick succession get ignored

Ask a motor to move again within a second or two of finishing, and it tends to
accept the command and then report nothing — indistinguishable from ignoring
it. Users of the Home Assistant integration reached the same conclusion
independently; sending requests in quick succession makes a blind do nothing at
all.

**Observed 2026-08-28.** Three blinds driven one after another: the first
command to each arrived and moved the rail, second commands sent seconds later
to two of them were accepted and never confirmed.

Consecutive commands to the same motor are therefore spaced.

### Moving several blinds at once is slow, and the reason is not obvious

Commanding three blinds together left `bank_top` needing three full discovery
rounds — about ninety seconds — before it was heard at -78 dBm.

The tempting explanation, that the scanner is stopped while other clients
connect, does not hold: discovery time is counted only while the scanner is
actually running, so a motor waiting its turn does not burn its budget. What
remains is that a radio which is scanning *and* servicing established
connections misses advertisements it would otherwise catch, and that this motor
is a weak advertiser to begin with.

Not addressed in firmware. A global cap on how many motors may be active at
once was considered and rejected: with a coordinator that needs both rails
before it can move, two blinds each holding one slot deadlock until their
operation deadlines expire. The effective lever is `optimistic`, which wakes
only the rails that actually move and so halves the radio load for the same
work.

### Rails move slowly, so travel budgets must be generous

A rail took four seconds to move a few percent. Budgets of seven and eight
seconds were declaring healthy moves failed. Being slow to give up costs a
delayed error message; being quick to give up throws away a working command.

### Battery readings are noisy

One motor reported 54, 43 and 51 percent within three minutes. Never conclude
anything from a single reading.

### Addresses are not all public

Of six motors seen here, the addresses spanned `DB:67:…`, `40:AC:…` and
`E0:BE:…`. The address type must be learned from an advertisement; assuming
public and connecting directly does not work. This is the main reason
connections are made by enabling a client and letting the tracker promote it
from an advertisement, rather than by calling `connect()`.

### Whether the timestamp is validated is still unknown

Every command carries a wall-clock timestamp. The reference library always
sends one, but nothing establishes that the motor checks it rather than
treating it as a nonce. `time_id` is required on the assumption that it
matters. Untested: a deliberately wrong time, and a repeated timestamp.

---

## ESPHome's BLE stack

### Scanning stops whenever any client is connecting

`esp32_ble_tracker.cpp` restarts the scanner only when no client is in
`CONNECTING`, `DISCOVERED` or `DISCONNECTING`. With several motors, one rail's
connection attempt therefore eats into another's chance of being discovered.

A discovery deadline measured on the wall clock is wrong for that reason; this
component counts only the time the scanner is actually running.

### Every client does see every advertisement

The dispatch loop calls all clients with no early exit, and `already_discovered_`
is used solely to deduplicate log lines — it does not suppress dispatch. If a
motor is not being discovered, the advertisement genuinely is not arriving.

### A pending connect cannot be abandoned

`esp_ble_gattc_open()` has no cancel through ESPHome's public API, and giving up
on it locally does not stop it — the base client stays in `CONNECTING`, and the
tracker will not scan while any client is. So a short per-attempt deadline on a
slow connect does not just fail that motor: it stalls discovery for every motor
on the node while the open is still outstanding. Only the long stuck-connect
deadline applies while an open is pending.

### `on_disconnect_complete()` is protected

It is reachable only from a `BLEClientBase` subclass, not from a `BLEClientNode`.
That, plus wanting one YAML block per motor, is why this component owns its own
client class rather than using the `ble_client` component.

### Services are freed once a node reports established

`BLEClient` calls `release_services()` as soon as all its nodes are established,
after which `get_characteristic()` returns null. Handles must be cached during
service discovery and re-acquired on every connection.

### The connection-slot check only warns

`esp32_ble.validate_connection_slots()` raises `cv.Invalid` only above the
generic IDF maximum of nine; below that it merely logs a warning. The real
ceiling is Bluedroid's `GATT_MAX_APPS`, which is 8 with two used internally —
**six client registrations**. Exceeding it builds cleanly and fails on the
device during app registration, which is why this component enforces six itself.

Note also that `esp32_ble: max_connections:` is the number of connections; ESPHome
adds the advertising/scanning instance on top of it. Six motors is
`max_connections: 6`, not seven.

### Preferences are only flushed on a clean shutdown

There is no periodic sync. Anything that reboots without getting that far
discards pending writes — which silently defeated position persistence until
each save was committed explicitly.

### The GATT database can be cached

`CONFIG_BT_GATTC_CACHE_NVS_FLASH` keeps a device's discovered attribute table in
flash, so reconnects skip the walk. `bluetooth_proxy` enables it; this component
does too.

---

## Home Assistant

### A cover position cannot say "unknown"

`Cover::Cover() : position{COVER_OPEN}` — an entity that has never published
still reports **100%**, because Home Assistant reads the member when it
enumerates entities. Not publishing does not prevent it. The only real defence
is to have a position worth publishing, which is why positions are persisted.

### `assumed_state` is sent once

It appears only in `ListEntitiesCoverResponse`, never in state updates, so it
cannot track freshness at runtime. That is why freshness is a separate
`binary_sensor` instead.

---

## Measurements

One run each, on one installation, 2026-08-28. Indicative, not a benchmark.

| | `optimistic: false` | `optimistic: true` |
| --- | --- | --- |
| command to rail stopped | 26.9 s | 15.1 s |
| of which connecting | 22.5 s, two motors | 10.8 s, one motor |
| of which moving | 4.4 s | 4.3 s |

The connection is the whole overhead. Nothing but keeping links open shortens
it further.

Signal strengths ranged from -63 to -91 dBm for the same motor within an hour,
so a single RSSI reading says little.

---

## Tried and rejected

**Scaling a rail's position against the other rail's remaining travel.** What
the gateway integration does. It makes a stationary rail's reported position
change whenever the other one moves — observed as a rail flapping 97/100/92/100
while untouched — and makes a requested 50% land somewhere different depending
on where the other rail is. Each rail is now scaled against its own travel.

**Making the top rail's cover position read the other way round** (v0.0.4), so
that Home Assistant's up arrow raised it. Defensible alone, but no other
top-down bottom-up shade behaves that way. Reverted to the gateway convention in
v0.0.5.

**Direct `connect()` with `auto_connect: false`.** Bypasses the tracker's
one-at-a-time serialisation and never learns the address type. See above.

**A minimum gap between the rails, by default.** These rails stack against each
other at both ends of the window; a 5% default clamped a fully open blind at
93% and made the clearance watchdog fire on a perfectly normal stacked
position. The default is now zero, and ordering does the work.

**Repeating the key and status query three times inside one connection**
(v0.0.14). One repeat is enough, and hammering a motor that has stopped
listening is reported to be counterproductive.
