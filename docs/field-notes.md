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

That was not enough on its own, and the reason is a third note from this same
page: the recheck's question is written without a response, and those writes get
lost. One unanswered attempt was condemning a rail that was sitting exactly
where it had been told to go. The question is now asked up to three times, the
way the handshake already asks its own status query more than once.

This only bites when the rail's position was not already known -- otherwise the
command is recognised as a no-op before it is sent. It therefore showed up only
once positions stopped surviving a restart, which is how two unrelated faults
came to look like one.

### Rails move slowly, so travel budgets must be generous

A rail took four seconds to move a few percent. Budgets of seven and eight
seconds were declaring healthy moves failed. Being slow to give up costs a
delayed error message; being quick to give up throws away a working command.

### A translation keeps the gap between the rails constant, so a gap rule never fires

The most consequential thing about the top-down bottom-up geometry, and the one
that is genuinely not obvious from the code: when the two rails move the same
physical direction by the same amount — sliding the fabric block down, which is
what the fabric-position number does — **the distance between them does not
change at any point during the move.**

The first version of the collision rule released the second rail once the
observed gap reached `start_gap`. Read that again against the sentence above: on
a translation that condition can never become true if it was not already true at
dispatch. The rule silently degraded into "the second rail waits out the whole of
the first rail's travel", every single time, and nothing in the log said so — it
looked like a safety wait doing its job.

The fix is not a shorter wait. It is a different question: not *is there room
yet* (there always was, the gap is constant) but *has the leading rail actually
left*, so the second one is following it rather than driving into something
stationary. One reported percent of the leading rail's own travel, in the
gap-opening direction, from a position observed on the current link.

The same reading disposes of the other half: when **both** rails open the gap —
opening or closing a collapsed blind, the most common command there is — every
intermediate state of both moves increases the distance between them, and
overshoot can only increase it further. There was never anything to wait for
there either.

### Clamping a target against a live position needs a residual completion

Found while making the second rail start earlier, but it was already there and
already reachable on defaults. Every rail's target is clamped so it cannot be
commanded past the other one. The reference used for that clamp was the other
rail's **live position** — which is right for the rail that goes first, and
wrong for the rail that follows, because the rail it is being held off is on its
way somewhere else.

Concretely, with `min_gap: 0%` and `safety_margin: 0%`: rails at 50 and 60,
translate the pair 20% down. Targets 70 and 80. Clamping the top rail's 70
against the bottom rail's live 60 yields **60**, not 70. Nothing re-issues the
remainder, so the blind ends the move with the wrong length *and* the wrong
centre, and reports success.

Two things made it easy to miss. It only bites when both rails are in flight at
once, which before this change happened only above `start_gap`. And the
exhaustive geometry test clamped each rail against the other's **target** — so
the test was modelling a different program from the one that shipped, and passed.

Using the leading rail's committed target looks tempting, but is unsafe if that
write is lost or the rail stalls: the follower would then be allowed to cross a
rail which never vacated its old position. The follower is therefore held off
the leading rail's latest **observed** position in every case except both rails
moving apart, where every interleaving increases the gap. This can deliberately
clip the follower's first command.

A bounded residual-completion pass supplies the rest safely: what each rail was
asked for is recorded alongside what it could actually be given, and if the two
still differ by more than one reported quantum once everything has stopped, the
remainder is issued once against the geometry as it now stands. Thus a stalled
leader cannot be crossed, while a healthy translation still reaches the full
requested position after at most one extra command per rail.

### One reported percent is half a second to a second of travel

The number that makes early release defensible. Rails move at roughly 1–2 %/s
(the four-seconds-for-a-few-percent measurement above), and the motors report
whole percents of their own travel, so the smallest movement that can be
observed at all corresponds to about half a second to a second of travel. The
clearance watchdog therefore reacts within roughly one percent of a breach —
which is the quantitative basis for letting the second rail start on one percent
of evidence rather than on the whole move.

For a rail calibrated over only part of the window, one raw percent is
correspondingly less window travel (0.4% on a rail spanning 40%). The floor that
carries the safety in that case is the required clearance, not the step size.

### The locally-set moving flag is a claim about us, not the rail

`moving_` is set by this node the instant it hands a command to the radio. It is
not feedback, and every decision that rested on it was really resting on "we
sent something". Two of them mattered: the trailing rail's release, and the
"leading rail has settled" test — the latter because the flag is also cleared
when a link drops, so a connection lost mid-move made a stale position within
1% of target read as "the lead is done", exactly when the coordinator had lost
its eyes.

What replaced it: an explicit "the position write has gone out" accessor for
attributing feedback to our own command, `busy()` plus a *freshly observed*
position on target for settlement, and observed movement for departure.

### A lost position write costs half a minute before anything is retried

Given that writes are unacknowledged and do get lost (above), it is worth
writing down what the failure actually costs. On a 20% move: 8 s base plus 700 ms
per percent of travel = 22 s of travel budget, then up to three status rechecks
five seconds apart, then one re-delivery. A command that never reached the motor
therefore sits there for roughly half a minute looking like a slow blind.

A rail that has not been reported at *any* other position a few seconds after
the write is a far earlier signal — but it is not proof, because a healthy motor
can still be inside its first reported percent, and a re-send landing on a moving
rail is precisely the "commands in quick succession get ignored" fault above.
So the early check asks rather than concludes: it seeks two matching status
answers with queries two seconds apart, and has one third query as loss reserve.
A re-send happens only when **two** answers name the exact position the command
was sent from. Three seconds is the floor because that is the
minimum spacing between any two commands to one motor; five is the default for
the first question because of the four-second observation above. The normal
retry therefore happens at roughly seven seconds.

Two properties make it safe to run by default. A wrong starting position — a
restored one that no longer holds — can only make the answer *differ* from it,
so it costs a missed retry and never a false one. And the early path has no
failing outcome at all: running out of questions is silence, not condemnation,
and only the post-travel path may fail a move. Both paths share the same
two-delivery budget, so an early retry consumes the only re-delivery and can
never be followed by a third position write later.

### Battery readings are noisy

One motor reported 54, 43 and 51 percent within three minutes. Never conclude
anything from a single reading.

### Addresses are not all public

Of six motors seen here, the addresses spanned `DB:67:…`, `40:AC:…` and
`E0:BE:…`. The address type must be learned from an advertisement; assuming
public and connecting directly does not work. This is the main reason
connections are made by enabling a client and letting the tracker promote it
from an advertisement, rather than by calling `connect()`.

Once learned, the address and type stay in the client for the rest of the boot.
The component can therefore put that client back into the tracker's
`DISCOVERED` queue without calling `connect()` itself. This preserves the
tracker's one-at-a-time promotion while letting the initiator wait for the next
advertisement immediately. A failed cached attempt invalidates the pair and
falls back to the advertisement-driven path.

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

### A legacy pending connect cannot be abandoned

`esp_ble_gattc_open()` has no cancel through ESPHome's base client, and giving
up on it locally does not stop it — the client stays in `CONNECTING`, and the
tracker will not scan while any client is. ESP-IDF 5.5.5's
`esp_ble_gattc_enh_open()` and `esp_ble_gattc_cancel_open()` close that hole.
This component uses them by default, cancels at `connect_timeout`, and keeps the
long stuck deadline only for a stack that never confirms the cancellation.

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

### A boot log cannot be read from Home Assistant or the dashboard

Anything logged from `setup()` is emitted before the API is up, so neither the
ESPHome dashboard (which connects over that API) nor Home Assistant's device-log
subscription ever sees it. Chasing a startup fault through those log lines is
therefore impossible; only a serial cable would show them.

State that matters at startup has to be reported from `dump_config()` instead,
which runs once the API is connected. Note that `ESP_LOGCONFIG` is compiled out
at log level INFO, so a condition someone needs to see while running at INFO
must be a warning, not a config line.

### Restored is not the same as published

A restored position that nobody publishes is indistinguishable from a lost one,
and that cost most of an evening. `setup()` read the stored value and logged it
correctly for every motor while every cover in Home Assistant still read 100% --
the value `Cover` is constructed with.

The cause is setup ordering. The coordinator registers its update callback on
each motor from its own `setup()`, and ESPHome does not promise which component
is set up first, so a publish from the motor's `setup()` can reach nobody. There
was no second chance: nothing republished until a BLE frame arrived, which on a
quiet blind can be hours.

State restored during setup therefore has to be announced from the first
`loop()` pass, once every component exists. The general shape is worth
remembering: anything held internally whose only route out depends on an event
that may never happen will eventually look like data loss.

### A preference key is a global 32-bit namespace, not a private one

Every component on a node shares one flat key space. This component derived its
key by folding the motor's address into an integer, which for a four-character
`mac_code` is a small number like 2650 -- and small numbers are exactly what a
collision looks like. A colliding key reads back either nothing, or another
component's bytes interpreted at this component's length.

Keys are now an FNV-1a hash of `motionblinds_ble:<version>:<identity>`, so they
are spread across the space and carry their own namespace and format version.

The identity is whichever of `mac_code` and `mac_address` was configured, and
those cannot be reconciled: the full address is not knowable from the code at
build time. Switching a motor from one to the other therefore loses its stored
position once. Refreshing the blind puts it back.

### A stored blob must say what it is

The blob had no version, no magic and no validation, and used `bool` fields --
which store whatever byte the compiler chooses and read back as true for
anything non-zero. It had already grown from three bytes to six, and the ESP32
backend rejects a length mismatch silently, so an upgrade became "nothing
stored" with no way to tell that apart from a first boot.

It now carries a version byte and fixed-width flags, and is range-checked before
it is believed: positions are percentages, tilt is an angle, and charging means
nothing without a battery reading to qualify. A blob that fails any of those did
not come from a motor, whatever wrote it.

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

### Withholding a cover publish does not make it unknown

The comments in both cover platforms used to claim that not publishing left the
entity unknown. It does not. `Cover` is constructed at fully open, and the
native API serialises a cover's position unconditionally with no missing-state
bit, so an unknown rail is exported as 100% regardless. That is why a restored
position that was never published looked identical to a lost one for most of an
evening.

The freshness binary sensor and the blind's status text are the entities that
can express "unknown". They are the ones to believe, and the ones an automation
should branch on -- noting that the status text reports *freshness*, which goes
false a few seconds after every disconnect, and not whether a position is known
at all.

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

A later `v0.0.28-beta1` test on the large blind, 2026-08-30, provided the first
end-to-end command timestamps: 7.254, 5.951, 0.596 and 9.906 seconds (mean
5.927). The three cold paths each spent about 5.1 seconds in `link`, after first
spending 0.6 to 4.5 seconds in `heard`; services were already 0.0 seconds and
notifications plus key about 0.3 seconds. The 0.596-second sample reused a live
link. This is the evidence behind cached connect: the old cold path waits for
one advertisement to be heard and then roughly one more inside the initiator.
It also bounds what remains after the radio work -- only a few tenths of a
second, unless the motor drops a write and invokes a deliberate retry.

---

## Ideas assessed and not taken

**Raising the *idle* scanner duty cycle.** Nothing to win on 2026.8: the window
is already raised to the interval. The version matters, though — it was 30 ms
against 320 ms in 2026.6, so advice about this is only as good as the version it
was written against. The duty cycle *during connections* is a different setting
and is very much worth raising; see above.

**Trusting a persisted address forever.** The cached pair removes a redundant
advertisement wait: instead of hearing one advertisement and then starting the
initiator for the next, the initiator starts immediately. Some motors use random
addresses, so the pair is not trusted indefinitely. A restored shortcut gets a
bounded attempt through cancellable enhanced open; failure erases it and returns
to advertisement discovery.

**A higher-duty initiator.** The legacy connection scan defaults to 30/60 ms.
Enhanced open accepts explicit create-connection parameters, so this component
uses 40/40 ms while the link is pending. Missing one advertisement can cost a
whole motor advertising cycle; listening continuously removes that avoidable
half-duty gap. This affects the ESP radio only during link establishment.

**MTU tuning and persisting raw handles.** The frames are tiny, MTU negotiation
already starts in the connect event and saves about three milliseconds, and the
GATT cache already removes rediscovery. No evidence that discovery rather than
advertisement capture owns the connection time.

**Preferred connection interval.** The legacy BLE client path used by this
component leaves ESP-IDF's 12.5–15 ms default in place. The pre-release now asks
for ESPHome's balanced 8.75–11.25 ms interval before opening the link. This is a
small GATT/handshake optimization, not a remedy for seconds spent waiting for
an advertisement, and is configurable as `low_latency_connection` in case a
motor rejects it.

**Keeping the link open.** A warm-path command measured 0.596 s, but buying that
for every command means leaving a battery motor connected. This was rejected:
the in-memory identity shortcut removes one advertisement cycle without adding
any connected time.

**Prewarming instead of pretending cold start can disappear.** The tracker must
still hear an advertisement and opens clients serially. For scheduled moves the
TDBU `prepare` button pays that cost beforehand and holds bounded leases; for a
close-gap move the trailing motor is only made eligible after feedback proves
the leader is physically moving, then connects while clearance opens. In both
cases a movement command remains subject to the same observed-clearance rule.

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
