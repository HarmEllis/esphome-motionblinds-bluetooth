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
actually running, so a motor waiting its turn does not burn its budget.

The real cause is below: while any motor is connected, ESPHome scans at a
9.4 % duty cycle instead of 100 %. A rail that has to be found while its
partner is still connected is looking through a tenth of the airtime, and these
motors are weak advertisers to begin with.

Not addressed in firmware. A global cap on how many motors may be active at
once was considered and rejected: with a coordinator that needs both rails
before it can move, two blinds each holding one slot deadlock until their
operation deadlines expire. The effective lever is `optimistic`, which wakes
only the rails that actually move and so halves the radio load for the same
work.

### The status query before the first command costs a round trip

A motor is keyed, then asked where it is, and only once it answers is the queued
command sent. On a healthy link that is a few hundred milliseconds. On a link
where the query write is lost — which does happen, see above — it is a three
second retry, sometimes twice.

`fast_connect` sends the waiting command straight after keying instead. The
guarantee that matters survives: the command is still only reported as done once
the motor confirms it. What is lost is the early proof that the key landed, so a
motor that was never keyed fails at its travel deadline instead of its handshake
deadline — later, and named less usefully.

Not made the default. A user who never sets it keeps the more precise failure
reporting, and a first move on a motor with no remembered position still takes
the slow path regardless, because the travel budget depends on knowing where the
rail started.

Caught on hardware, not in review: skipping the query also skips the only frame
that carries battery, speed and favourite. Feedback frames during a move do not
have them, so the first fast_connect move left the battery on `unknown` --
precisely the fault this component was built to escape. The query is now sent
after the work instead of before it, and only when those values are over an hour
old, so it uses idle time rather than adding to the wait.

### A rail already at its target used to fail every time

Two field notes combine into a bug. A motor at its target says nothing, and
arrival is normally only accepted after two frames agree. So a command to a
position the rail is already standing on gets no frames at all, runs out its
travel budget, and reaches the recheck -- which asks the motor where it is,
receives one status frame saying exactly the commanded position, and rejects it
for being a single frame. Five seconds later the command is condemned with
"motor never reached the commanded position".

Observed twice within a minute on a rail that was already up. The two-frame rule
exists so a position the rail is passing through cannot be mistaken for arrival;
after a recheck there is nothing to pass through, because the rail has stood
still for the whole budget and the frame is the answer to a question that was
asked deliberately. The recheck's answer is now accepted on its own.

This only bites when the rail's position was not already known -- otherwise the
command is recognised as a no-op before it is sent. It therefore showed up only
once positions stopped surviving a restart, which is how two unrelated faults
came to look like one.

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

### The scanner drops to a 9.4 % duty cycle while any motor is connected

Measured on a real move: a rail was heard 1.5 s after its partner disconnected,
having gone unheard for the fifteen seconds before that while the partner was
connected.

Since 2026.8, when ESP-IDF is at least 5.5.5 and software coexistence is
compiled in — which is the case whenever `wifi:` is configured — ESPHome does
two things to a `scan_parameters:` block that leaves `window:` unset
(`esp32_ble_tracker/__init__.py`, `_raise_defaulted_scan_window`):

* it raises the scan window to the interval, so idle scanning runs at 320/320 ms
  — a full 100 % duty cycle;
* it injects `connection_scan_window: 30ms`, and
  `desired_scan_window_(active)` selects that value **whenever at least one
  client is connected**.

So the node scans at 100 % while idle and at 30/320 ms — 9.4 % — for exactly the
period this component depends on most: finding the second rail while the first
one is still connected and leased for the duration of the move.

Connected clients do not block the scanner (only `CONNECTING`, `DISCOVERED` and
`DISCONNECTING` do), so the scanner really is running the whole time; it is
simply listening for a tenth of it. The tracker restarts the scan as soon as the
programmed window stops matching the connection count, so the switch is not
deferred to the end of a scan period.

Raising it is a one-line change in the node's YAML:

```yaml
esp32_ble_tracker:
  scan_parameters:
    continuous: true
    connection_scan_window: 160ms   # 50 %; the default injects 30ms (9.4 %)
```

The value must be no larger than the scan window. Upstream's 30 ms is a
deliberate, sensible default for a general-purpose node, where a full-duty scan
during a live connection would starve Wi-Fi and the connection events
themselves; a node whose only job is these motors can afford more. 160 ms is a
starting point, not a measured optimum — raise it in steps and watch for lost
status frames, handshake retries and API reconnects, not just for faster
discovery.

On ESPHome before 2026.8 the option does not exist, and the scan window is
30 ms at all times.

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

The connection is the whole overhead. Two things do shorten it without keeping
links open: raising `connection_scan_window`, and `fast_connect`.

Phase breakdown from the connection-status history of a real boot, six motors
across two nodes:

| | heard after | then ready after |
| --- | --- | --- |
| tv bottom, nothing else connected | 0.6 s | 9.0 s |
| tv bottom, second attempt | 5.4 s | 2.8 s |
| tv top, partner connected throughout | 26.2 s | 5.2 s |
| bank bottom, nothing else connected | 0.5 s | 8.9 s |
| bank bottom, second attempt | 18.8 s | 4.8 s |

Two separate problems, and they need separate fixes. Being *heard* varies from
half a second to twenty-six, and tracks whether another motor is connected —
that is the scan window. Getting from heard to ready is a fairly steady three to
nine seconds even with an idle radio, and that is the link, GATT and the motor's
own response time. The per-phase log line exists so the next measurement does
not have to be inferred from state history like this one was.

Signal strengths ranged from -63 to -91 dBm for the same motor within an hour,
so a single RSSI reading says little. Moving a node physically closer to the
weakest motor did not improve its reading: -91 and -86 dBm on a board sitting
beside the blind, against -87 dBm from a node across the room. A worse antenna
gives back what proximity wins.

---

## Ideas assessed and not taken

**Raising the *idle* scanner duty cycle.** Nothing to win on 2026.8: the window
is already raised to the interval. The version matters, though — it was 30 ms
against 320 ms in 2026.6, so advice about this is only as good as the version it
was written against. The duty cycle *during connections* is a different setting
and is very much worth raising; see above.

**Direct connect with a cached address and type.** Tempting, since waiting for
an advertisement is the slow part. It does not help: `esp_ble_gattc_open()`
still waits for the target's next advertisement, but does so inside the
uncancellable `CONNECTING` state — moving the wait to the one place where it
blocks every other motor on the node.

**MTU tuning and persisting raw handles.** The frames are tiny, MTU negotiation
already starts in the connect event and saves about three milliseconds, and the
GATT cache already removes rediscovery. No evidence that discovery rather than
advertisement capture owns the connection time.

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
