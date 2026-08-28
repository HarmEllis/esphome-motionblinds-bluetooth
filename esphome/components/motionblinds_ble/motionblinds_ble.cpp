#include "motionblinds_ble.h"

#ifdef USE_ESP32

#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/log.h"

namespace esphome::motionblinds_ble {

static const char *const TAG = "motionblinds_ble";

/// The motor needs a moment after SET_KEY before it answers; curtain and
/// vertical blinds need considerably longer.
static const uint32_t SETTLE_DELAY_MS = 200;
static const uint32_t CURTAIN_SETTLE_DELAY_MS = 500;
/// A write is only unacknowledged for as long as the local stack takes.
static const uint32_t COMMAND_ACK_TIMEOUT_MS = 5000;
/// Travel budget, scaled by how far the rail has to go.
///
/// Generous on purpose. Field logs show a rail taking four seconds to move a
/// few percent, and a motor that has just finished a move pauses before it
/// answers again — budgets of seven and eight seconds were declaring perfectly
/// healthy moves failed. Being slow to give up costs a delayed error message;
/// being quick to give up throws away a working command.
static const uint32_t TRAVEL_TIMEOUT_BASE_MS = 8000;
static const uint32_t TRAVEL_TIMEOUT_PER_PERCENT_MS = 700;
/// Least time between two commands to the same motor.
///
/// A motor asked to move again immediately after finishing tends to accept the
/// command and then report nothing, which is indistinguishable from ignoring
/// it. Users of the integration this replaces reached the same conclusion
/// independently: sending requests in quick succession makes a blind do
/// nothing at all.
///
/// Three seconds is a starting point rather than a measurement. It is the only
/// interval with any evidence behind it — the same one the library's own
/// maintainer chose between status-query retries — and the failures seen here
/// were all well inside it. If second commands still go unconfirmed, this is
/// the first number to raise.
static const uint32_t MIN_COMMAND_GAP_MS = 3000;
static const uint8_t MAX_ATTEMPTS = 3;
static const uint8_t QUEUE_LIMIT = 8;
/// Two consecutive frames on target before a move counts as settled, so a
/// single frame taken while the rail passes through cannot end the wait.
static const uint8_t SETTLE_FRAMES = 2;
/// How long to wait for a status frame before asking again, and how many times.
///
/// Three attempts three seconds apart, matching the fix the library's own
/// maintainer proposed for this exact fault (LennP/motionblindsble#5, against
/// home-assistant/core#153218). Only the query is repeated — that PR does not
/// re-send the key, and neither does this.
/// How stale battery, speed and favourite may get before a fast_connect
/// session spends a query on refreshing them. They change over days, so this is
/// deliberately long: the query holds the link open, and a held link is exactly
/// what makes the next motor slow to find.
static const uint32_t STATUS_REFRESH_MS = 3600000;

/// How often the "where are you really" question is asked before a move is
/// declared failed. More than once because the question is written without a
/// response, and those writes do get lost.
static const uint8_t MAX_SETTLE_RECHECKS = 3;

static const uint32_t HANDSHAKE_RETRY_MS = 3000;
static const uint8_t MAX_HANDSHAKE_ATTEMPTS = 3;

const char *motor_state_to_string(MotorState state) {
  switch (state) {
    case MotorState::IDLE:
      return "disconnected";
    case MotorState::DISCOVERING:
      return "discovering";
    case MotorState::CONNECTING:
      return "connecting";
    case MotorState::HANDSHAKE:
      return "connecting";
    case MotorState::READY:
      return "connected";
    case MotorState::DISCONNECTING:
      return "disconnecting";
    case MotorState::FAILED:
      return "error";
  }
  return "unknown";
}

void MotionblindsBLEMotor::setup() {
  this->state_pref_ = global_preferences->make_preference<PersistedState>(this->preference_key_);

  PersistedState stored{};
  this->restored_ = this->state_pref_.load(&stored);
  if (this->restored_) {
    // Restored for continuity only. Explicitly not fresh: a remote or the
    // vendor app can move a rail while this node is powered off, so a restored
    // position must never be the basis for a collision decision.
    if (stored.has_position) {
      this->raw_position_ = stored.raw_position;
      this->raw_tilt_ = stored.raw_tilt;
      this->has_position_ = true;
      this->position_fresh_ = false;
    }
    // A battery percentage moves over days, so yesterday's reading beats no
    // reading at all — and it is the number people check when a blind stops
    // responding.
    if (stored.has_battery) {
      this->battery_percentage_ = stored.battery_percentage;
      this->battery_charging_ = stored.charging;
      this->has_battery_ = true;
    }
    ESP_LOGI(TAG, "[%s] Restored position %u and battery %u%% from before the restart", this->label_,
             static_cast<unsigned>(this->raw_position_), static_cast<unsigned>(this->battery_percentage_));
  } else {
    // Silence here used to be indistinguishable from "restored nothing
    // interesting", which made it impossible to tell a motor that had never
    // been heard from a store that was not being written.
    ESP_LOGI(TAG, "[%s] Nothing stored from before the restart", this->label_);
  }
}

void MotionblindsBLEMotor::dump_config() {
  ESP_LOGCONFIG(TAG, "Motionblinds BLE motor");
  ESP_LOGCONFIG(TAG,
                "  Window range: %.1f%% - %.1f%%\n"
                "  Inverted: %s\n"
                "  Disconnect delay: %us\n"
                "  Discovery timeout: %us\n"
                "  Fast connect: %s\n"
                "  Recover by reboot: %s",
                this->range_.window_min, this->range_.window_max, YESNO(this->range_.invert),
                static_cast<unsigned>(this->disconnect_delay_ / 1000),
                static_cast<unsigned>(this->discovery_timeout_ / 1000), YESNO(this->fast_connect_),
                YESNO(this->recover_by_reboot_));
  // Reported here rather than from setup(), which runs before the API is up
  // and can therefore be read from neither Home Assistant nor the ESPHome
  // dashboard. At INFO rather than CONFIG for the same kind of reason: config
  // lines are compiled out at INFO, which is where people actually run.
  //
  // Three outcomes, not two. "No blob" and "a blob that says nothing is known"
  // are different faults with different causes, and telling them apart from the
  // outside was impossible while both were silent.
  if (!this->restored_) {
    ESP_LOGW(TAG, "[%s] Flash store held nothing; this rail does not know where it is", this->label_);
  } else if (!this->has_position_) {
    ESP_LOGW(TAG, "[%s] Flash store was read but holds no position; this rail does not know where it is",
             this->label_);
  } else {
    if (this->has_battery_) {
      ESP_LOGI(TAG, "[%s] Restored position %u, battery %u%%", this->label_, static_cast<unsigned>(this->raw_position_),
               static_cast<unsigned>(this->battery_percentage_));
    } else {
      ESP_LOGI(TAG, "[%s] Restored position %u, no battery reading", this->label_,
               static_cast<unsigned>(this->raw_position_));
    }
  }
  if (this->time_ == nullptr)
    ESP_LOGE(TAG, "  No time source: commands cannot be encrypted");
}

// ---------------------------------------------------------------- requests

bool MotionblindsBLEMotor::request_position(float window_position) {
  if (std::isnan(window_position))
    return false;
  const uint8_t raw = static_cast<uint8_t>(lroundf(this->range_.to_raw(window_position)));
  return this->enqueue_(Command::PERCENT, raw, Verification::SETTLED, raw);
}

bool MotionblindsBLEMotor::request_open() {
  // Sent as an explicit position rather than the protocol's OPEN command:
  // OPEN means "fully open in the motor's own frame", which is the wrong end
  // of the window for an inverted or partially calibrated rail.
  return this->request_position(this->range_.invert ? this->range_.window_max : this->range_.window_min);
}

bool MotionblindsBLEMotor::request_close() {
  return this->request_position(this->range_.invert ? this->range_.window_min : this->range_.window_max);
}

bool MotionblindsBLEMotor::request_stop() {
  // Stop is the safety primitive: the clearance watchdog calls it to break up
  // an imminent collision, so it must never queue behind the position command
  // it is trying to interrupt. Waiting for that command to verify would mean
  // stopping only once the rail had already arrived.
  this->queue_.clear();
  this->command_in_flight_ = false;
  this->settle_matches_ = 0;
  this->moving_ = false;
  this->travel_direction_ = 0;
  this->publish_();

  if (this->state_ == MotorState::READY && this->command_handle_ != 0) {
    if (this->write_command_(Command::STOP, 0)) {
      this->in_flight_ = PendingCommand{Command::STOP, 0, Verification::ACKED, 0};
      this->command_in_flight_ = true;
      this->command_sent_at_ = millis();
      this->command_budget_ = COMMAND_ACK_TIMEOUT_MS;
      this->last_activity_ = this->command_sent_at_;
      // A stop leaves the rail wherever it happened to halt, which is not the
      // position anything still believes. Read it back before the link closes,
      // otherwise the last commanded target stands as the reported state.
      this->enqueue_(Command::STATUS_QUERY, 0, Verification::STARTED);
      return true;
    }
    ESP_LOGW(TAG, "[%s] Could not write stop directly, queueing it", this->label_);
  }

  // Not connected, or the write failed: fall back to the queue so the stop
  // still goes out once there is a link.
  return this->enqueue_(Command::STOP, 0, Verification::ACKED);
}

bool MotionblindsBLEMotor::request_favorite() {
  if (!this->favorite_set_ && this->position_fresh_) {
    ESP_LOGW(TAG, "[%s] No favorite position is set; refusing a command it would ignore", this->label_);
    return false;
  }
  return this->enqueue_(Command::FAVORITE, 0, Verification::ACKED);
}

bool MotionblindsBLEMotor::request_speed(SpeedLevel level) {
  return this->enqueue_(Command::SPEED, static_cast<uint8_t>(level), Verification::ACKED);
}

bool MotionblindsBLEMotor::request_status() { return this->enqueue_(Command::STATUS_QUERY, 0, Verification::STARTED); }

void MotionblindsBLEMotor::request_connect() {
  this->acquire_lease();
  this->request_status();
}

void MotionblindsBLEMotor::request_disconnect() {
  this->lease_count_ = 0;
  this->queue_.clear();
  this->abort_();
}

bool MotionblindsBLEMotor::enqueue_(Command command, uint8_t argument, Verification verification, uint8_t target) {
  if (this->time_ == nullptr) {
    ESP_LOGE(TAG, "[%s] Refusing command: no time source configured", this->label_);
    return false;
  }

  // A newer position request replaces an older one that has not gone out yet;
  // queueing both would drive the rail to a position nobody still wants.
  if (command == Command::PERCENT) {
    for (auto &queued : this->queue_) {
      if (queued.command == Command::PERCENT) {
        queued.argument = argument;
        queued.target = target;
        queued.verification = verification;
        this->start_operation_();
        return true;
      }
    }
  }

  if (this->queue_.size() >= QUEUE_LIMIT) {
    ESP_LOGW(TAG, "[%s] Command queue full, dropping command %d", this->label_, static_cast<int>(command));
    return false;
  }

  this->queue_.push_back(PendingCommand{command, argument, verification, target});
  this->start_operation_();
  return true;
}

void MotionblindsBLEMotor::start_operation_() {
  const uint32_t now = millis();
  this->last_activity_ = now;
  if (this->state_ == MotorState::FAILED) {
    // A new request is an explicit instruction to try again.
    this->attempts_ = 0;
    this->set_state_(MotorState::IDLE);
  }
  if (this->operation_since_ == 0)
    this->operation_since_ = now;

  if (this->state_ == MotorState::IDLE && this->ble_client_ != nullptr) {
    ESP_LOGD(TAG, "[%s] Work queued, listening for it", this->label_);
    this->ble_client_->set_enabled(true);
    this->discovery_round_ = 0;
    this->discovery_scanning_ms_ = 0;
    this->discovery_last_tick_ = now;
    this->backoff_until_ = 0;
    this->set_state_(MotorState::DISCOVERING);
    this->connecting_since_ = 0;
  }
}

void MotionblindsBLEMotor::finish_operation_() {
  this->operation_since_ = 0;
  this->attempts_ = 0;
  this->last_command_finished_ = millis();
}

void MotionblindsBLEMotor::acquire_lease() {
  this->lease_count_++;
  this->last_activity_ = millis();
  if (this->state_ == MotorState::IDLE)
    this->start_operation_();
}

void MotionblindsBLEMotor::release_lease() {
  if (this->lease_count_ > 0)
    this->lease_count_--;
  this->last_activity_ = millis();
}

// ------------------------------------------------------------------- loop

void MotionblindsBLEMotor::loop() {
  const uint32_t now = millis();

  // Announce the restored state once, here rather than from setup(). Components
  // are set up in an order this one does not control, and the coordinator
  // registers its update callback in its own setup(), so a publish from ours
  // can reach nobody. Nothing published it afterwards either, which left a
  // motor holding a perfectly good restored position while its cover sat on the
  // 100% every cover starts at -- restored and invisible, which reads exactly
  // like not restored at all.
  //
  // Gated on the whole application being set up rather than on merely reaching
  // loop(): ESPHome only promises that a component's own setup precedes its own
  // loop, and its slow-setup path will run an early component's loop() while
  // others are still being set up. Today's priorities happen to make that
  // harmless, but a setup_priority override in YAML would not.
  if (!this->announced_ && App.is_setup_complete()) {
    this->announced_ = true;
    this->publish_();
  }

  this->reconcile_state_();

  switch (this->state_) {
    case MotorState::IDLE:
    case MotorState::FAILED:
      break;

    case MotorState::DISCOVERING: {
      if (this->backoff_until_ != 0) {
        if (now < this->backoff_until_)
          break;  // deliberately quiet between rounds
        this->backoff_until_ = 0;
        this->discovery_scanning_ms_ = 0;
        this->discovery_last_tick_ = now;
        this->ble_client_->set_enabled(true);
        ESP_LOGD(TAG, "[%s] Discovery round %u of %u", this->label_, static_cast<unsigned>(this->discovery_round_ + 1),
                 static_cast<unsigned>(this->discovery_rounds_));
      }

      // The tracker stops scanning whenever any client is connecting, so wall
      // clock time overstates how long we actually listened. Counting only the
      // scanning time is what stops one motor's connection attempt from eating
      // another motor's discovery window.
      const bool scanning =
          esp32_ble_tracker::global_esp32_ble_tracker != nullptr &&
          esp32_ble_tracker::global_esp32_ble_tracker->get_scanner_state() == esp32_ble_tracker::ScannerState::RUNNING;
      if (scanning && this->discovery_last_tick_ != 0)
        this->discovery_scanning_ms_ += now - this->discovery_last_tick_;
      this->discovery_last_tick_ = now;

      if (this->discovery_scanning_ms_ <= this->discovery_timeout_)
        break;

      this->discovery_round_++;
      if (this->discovery_round_ >= this->discovery_rounds_) {
        this->fail_("never heard it advertise");
        break;
      }

      // A learned address that no longer answers is worse than none: drop it so
      // the code can be matched afresh.
      if (this->discovery_round_ >= 2)
        this->ble_client_->forget_address();

      ESP_LOGW(TAG, "[%s] Not heard in %us of scanning, retrying (round %u of %u)", this->label_,
               static_cast<unsigned>(this->discovery_scanning_ms_ / 1000),
               static_cast<unsigned>(this->discovery_round_ + 1), static_cast<unsigned>(this->discovery_rounds_));
      this->ble_client_->set_enabled(false);
      this->backoff_until_ = now + static_cast<uint32_t>(this->discovery_round_) * 5000;
      break;
    }

    case MotorState::CONNECTING: {
      const bool open_pending =
          this->ble_client_ != nullptr && this->ble_client_->state() == espbt::ClientState::CONNECTING;

      if (!open_pending) {
        // The link is up; what is left is service discovery, which either
        // completes or errors out, so an ordinary deadline is safe here.
        if (now - this->state_since_ > this->connect_timeout_)
          this->fail_("connected, but service discovery never finished");
        break;
      }

      // An outstanding esp_ble_gattc_open cannot be cancelled through the
      // public API, and giving up on it does not stop it: the base client stays
      // in CONNECTING, and the tracker will not scan while any client is. So
      // failing at the shorter connect deadline punished every motor on the
      // node for one slow connect, and left the reboot recovery below
      // permanently out of reach. While the open is pending, the stuck deadline
      // is the only one that applies.
      if (this->connecting_since_ == 0 || now - this->connecting_since_ <= this->stuck_connect_timeout_)
        break;

      if (!this->recover_by_reboot_) {
        this->fail_("stuck connecting, no connection event from the stack");
        break;
      }
      if (!this->stuck_reported_) {
        ESP_LOGE(TAG, "[%s] Stuck connecting with no way to cancel it; will reboot if it does not clear", this->label_);
        this->stuck_reported_ = true;
        this->publish_();
      }
      if (now - this->connecting_since_ > this->recover_after_) {
        ESP_LOGE(TAG, "[%s] Stuck connecting for %us, rebooting to recover", this->label_,
                 static_cast<unsigned>((now - this->connecting_since_) / 1000));
        App.safe_reboot();
      }
      break;
    }

    case MotorState::HANDSHAKE:
      if (now - this->state_since_ > this->handshake_timeout_) {
        // Name the step. "Handshake timed out" cannot distinguish a motor that
        // refused to enable notifications from one that was keyed and then
        // never answered the status query, and those have different causes.
        switch (this->handshake_) {
          case Handshake::WAIT_NOTIFY_REGISTRATION:
            this->fail_("motor never confirmed the notification registration");
            break;
          case Handshake::WAIT_DESCRIPTOR_WRITE:
            this->fail_("motor never enabled its notifications");
            break;
          case Handshake::WAIT_BLIND_SETTLE:
            this->fail_("motor did not settle after being keyed");
            break;
          case Handshake::WAIT_STATUS:
            // The signature of a connection this motor is not really present
            // on: linked and keyed, but silent, with no battery or speed ever
            // arriving. It is a known and still-open fault in the Home
            // Assistant integration this replaces, and the remedy people have
            // found is not patience but a fresh connection — typically two to
            // four before one takes. So this is retried like a dropped link
            // rather than treated as the motor's final answer.
            this->retry_connection_("keyed but silent");
            return;
          default:
            this->fail_("handshake timed out");
            break;
        }
      } else {
        this->drive_handshake_();
      }
      break;

    case MotorState::READY:
      this->dispatch_();
      // Only a STATUS frame carries battery, speed and favourite; the FEEDBACK
      // frames a move produces do not. So a fast_connect session that did
      // nothing but move would leave those blank -- which is the very symptom
      // this component was written to get away from. Ask once, after the work
      // is done and before the link is dropped, so it costs waiting time the
      // motor was going to spend idle anyway.
      if (this->fast_connect_ && !this->status_seen_ && !this->status_backfilled_ && this->queue_.empty() &&
          !this->command_in_flight_ &&
          (!this->ever_status_ || now - this->last_status_at_ > STATUS_REFRESH_MS)) {
        this->status_backfilled_ = true;
        this->request_status();
        break;
      }
      if (this->queue_.empty() && !this->command_in_flight_ && !this->leased() &&
          now - this->last_activity_ > this->disconnect_delay_) {
        ESP_LOGD(TAG, "[%s] Idle, disconnecting", this->label_);
        this->finish_operation_();
        this->abort_();
      }
      break;

    case MotorState::DISCONNECTING:
      // BLEClientBase has its own ten second watchdog for a lost CLOSE event
      // and forwards the resulting IDLE to us, so this only has to notice that
      // it never arrived at all.
      if (now - this->state_since_ > 15000) {
        ESP_LOGW(TAG, "[%s] Disconnect did not complete, giving up on it", this->label_);
        this->set_state_(MotorState::IDLE);
        this->mark_stale_();
      }
      break;
  }

  // An operation with nothing outstanding is over, whatever the state machine is
  // doing. Leaving the clock running meant a later request could inherit a start
  // time from minutes earlier -- start_operation_() only sets it when it is zero
  // -- and blow a deadline it never had a chance to meet. That is how a command
  // that had just failed for its own reason was immediately failed a second
  // time, with a different and misleading reason.
  if (this->operation_since_ != 0 && this->queue_.empty() && !this->command_in_flight_ && !this->leased())
    this->operation_since_ = 0;

  // A single operation may not outlive its total budget no matter which state
  // it is spread across.
  // The stuck-connect case is exempt: it is already counting down to its own
  // recovery, and failing it here would take that away.
  if (this->operation_since_ != 0 && now - this->operation_since_ > this->operation_timeout_ &&
      this->state_ != MotorState::FAILED && !this->stuck_reported_) {
    ESP_LOGE(TAG, "[%s] Operation ran for %us against a budget of %us", this->label_,
             static_cast<unsigned>((now - this->operation_since_) / 1000),
             static_cast<unsigned>(this->operation_timeout_ / 1000));
    this->fail_("operation exceeded its total deadline");
  }
}

void MotionblindsBLEMotor::reconcile_state_() {
  if (this->ble_client_ == nullptr)
    return;

  const espbt::ClientState client = this->ble_client_->state();

  if (client == espbt::ClientState::CONNECTING && this->connecting_since_ == 0)
    this->connecting_since_ = millis();

  switch (this->state_) {
    case MotorState::DISCOVERING:
      if (client == espbt::ClientState::CONNECTING || client == espbt::ClientState::CONNECTED ||
          client == espbt::ClientState::ESTABLISHED)
        this->set_state_(MotorState::CONNECTING);
      break;

    case MotorState::CONNECTING:
    case MotorState::HANDSHAKE:
    case MotorState::READY:
      // Every path that returns the parent to IDLE goes through the virtual
      // set_state(), which BLEClient forwards to its nodes, so this is a
      // reliable signal that the link is gone.
      if (client == espbt::ClientState::IDLE) {
        ESP_LOGD(TAG, "[%s] Connection lost", this->label_);
        this->mark_stale_();
        this->set_state_(MotorState::IDLE);
        this->connecting_since_ = 0;
        if (!this->queue_.empty() || this->leased()) {
          if (++this->attempts_ >= MAX_ATTEMPTS) {
            this->fail_("gave up after repeated connection failures");
          } else {
            ESP_LOGW(TAG, "[%s] Retrying, attempt %u of %u", this->label_, static_cast<unsigned>(this->attempts_ + 1),
                     static_cast<unsigned>(MAX_ATTEMPTS));
            this->ble_client_->set_enabled(true);
            this->set_state_(MotorState::DISCOVERING);
          }
        }
      }
      break;

    case MotorState::DISCONNECTING:
      if (client == espbt::ClientState::IDLE) {
        this->set_state_(MotorState::IDLE);
        this->connecting_since_ = 0;
      }
      break;

    default:
      break;
  }
}

void MotionblindsBLEMotor::log_connect_phases_() {
  if (this->phase_work_at_ == 0)
    return;  // already connected when the work arrived; nothing was paid

  const uint32_t now = millis();
  const auto secs = [](uint32_t from, uint32_t to) { return static_cast<double>(to - from) / 1000.0; };

  if (this->phase_heard_at_ == 0 || this->phase_open_at_ == 0 || this->phase_services_at_ == 0 ||
      this->phase_notify_at_ == 0) {
    ESP_LOGI(TAG, "[%s] Ready %.1fs after being wanted", this->label_, secs(this->phase_work_at_, now));
    this->phase_work_at_ = 0;
    return;
  }

  // Named separately because they have different remedies: being heard is the
  // scanner's duty cycle, the link is the controller's initiator, services are
  // the GATT cache, and the last two are the motor answering.
  ESP_LOGI(TAG,
           "[%s] Ready %.1fs after being wanted: heard %.1fs, link %.1fs, services %.1fs, notifications %.1fs, "
           "key %.1fs",
           this->label_, secs(this->phase_work_at_, now), secs(this->phase_work_at_, this->phase_heard_at_),
           secs(this->phase_heard_at_, this->phase_open_at_), secs(this->phase_open_at_, this->phase_services_at_),
           secs(this->phase_services_at_, this->phase_notify_at_), secs(this->phase_notify_at_, now));
  this->phase_work_at_ = 0;
}

void MotionblindsBLEMotor::drive_handshake_() {
  const uint32_t now = millis();

  if (this->handshake_ == Handshake::WAIT_BLIND_SETTLE) {
    const uint32_t delay = (this->blind_type_ == BlindType::CURTAIN || this->blind_type_ == BlindType::VERTICAL)
                               ? CURTAIN_SETTLE_DELAY_MS
                               : SETTLE_DELAY_MS;
    if (now - this->settle_since_ < delay)
      return;

    // With fast_connect, a motor that already has work waiting does not pay for
    // a status round trip before it can be given that work. The command's own
    // verification still has to prove the motor acted, so nothing is reported
    // as done that was not; what is given up is learning early that the key
    // never landed. That failure then surfaces as a travel timeout instead of a
    // handshake one — later, and with a less precise name.
    //
    // A position command is held back when no position is remembered: its
    // travel budget is derived from the distance to cover, and a budget
    // measured from a position that was never observed can cut a legitimate
    // move short.
    if (this->fast_connect_ && !this->queue_.empty() &&
        (this->queue_.front().command != Command::PERCENT || this->has_position_)) {
      this->handshake_ = Handshake::DONE;
      this->set_state_(MotorState::READY);
      this->attempts_ = 0;
      this->log_connect_phases_();
      ESP_LOGI(TAG, "[%s] Keyed with work waiting, skipping the status query", this->label_);
      return;
    }

    if (!this->write_command_(Command::STATUS_QUERY, 0)) {
      this->fail_("could not request status");
      return;
    }
    this->handshake_ = Handshake::WAIT_STATUS;
    this->handshake_retry_at_ = now + HANDSHAKE_RETRY_MS;
    return;
  }

  if (this->handshake_ != Handshake::WAIT_STATUS || now < this->handshake_retry_at_)
    return;

  // Every command is written without a response, so a lost write is silent.
  // Sitting out the whole handshake timeout waiting for an answer to a request
  // that never arrived wastes the connection; ask again instead. The key is
  // re-sent with it, because a motor that never got keyed would ignore the
  // query no matter how often it is repeated.
  if (++this->handshake_attempts_ >= MAX_HANDSHAKE_ATTEMPTS)
    return;  // let the handshake deadline name the failure

  ESP_LOGI(TAG, "[%s] No status yet, asking again (attempt %u of %u)", this->label_,
           static_cast<unsigned>(this->handshake_attempts_ + 1), static_cast<unsigned>(MAX_HANDSHAKE_ATTEMPTS));
  this->write_command_(Command::STATUS_QUERY, 0);
  this->handshake_retry_at_ = now + HANDSHAKE_RETRY_MS;
}

void MotionblindsBLEMotor::dispatch_() {
  const uint32_t now = millis();

  if (this->command_in_flight_) {
    switch (this->in_flight_.verification) {
      case Verification::ACKED:
        if (now - this->command_sent_at_ > this->command_budget_) {
          ESP_LOGW(TAG, "[%s] No write completion, treating the command as delivered anyway", this->label_);
          this->command_in_flight_ = false;
          this->finish_operation_();
        }
        break;

      case Verification::STARTED:
        if (now - this->command_sent_at_ <= this->command_budget_)
          break;
        // Asked again rather than failed outright, for the same reason the
        // handshake repeats its own status query: the question is a write
        // without a response and those get lost. This path had no retry at all,
        // which was harmless while the handshake always asked first -- but
        // fast_connect skips that, so a refresh button on a lossy link became a
        // single unanswered question and an error.
        if (this->recheck_attempts_ >= MAX_SETTLE_RECHECKS) {
          this->fail_("motor did not answer the status query");
          break;
        }
        ESP_LOGI(TAG, "[%s] No answer to the status query, asking again (attempt %u of %u)", this->label_,
                 static_cast<unsigned>(this->recheck_attempts_ + 1), static_cast<unsigned>(MAX_SETTLE_RECHECKS));
        this->recheck_attempts_++;
        this->command_sent_at_ = now;
        if (!this->write_command_(this->in_flight_.command, this->in_flight_.argument))
          this->fail_("motor did not answer the status query");
        break;

      case Verification::SETTLED:
        if (now - this->command_sent_at_ <= this->command_budget_)
          break;
        if (this->recheck_attempts_ >= MAX_SETTLE_RECHECKS) {
          this->fail_("motor never reached the commanded position");
          break;
        }
        // Running out of travel time is not proof that the rail did not get
        // there. These motors report when something changes, so a move that
        // finished quietly, or one whose remembered start position was wrong,
        // looks identical to one that never happened. Ask before condemning it.
        //
        // Asked more than once, because the question is written without a
        // response and a lost write is silent -- the same reason the handshake
        // repeats its status query. A single unanswered attempt was condemning
        // rails that were sitting exactly where they had been told to go.
        ESP_LOGI(TAG, "[%s] No arrival reported after %us, asking where it is (attempt %u of %u)", this->label_,
                 static_cast<unsigned>(this->command_budget_ / 1000),
                 static_cast<unsigned>(this->recheck_attempts_ + 1),
                 static_cast<unsigned>(MAX_SETTLE_RECHECKS));
        this->recheck_attempts_++;
        this->settle_rechecked_ = true;
        this->command_sent_at_ = now;
        this->command_budget_ = COMMAND_ACK_TIMEOUT_MS;
        if (!this->write_command_(Command::STATUS_QUERY, 0))
          this->fail_("motor never reached the commanded position");
        break;
    }
    return;
  }

  if (this->queue_.empty())
    return;

  // Give the motor room between commands; see MIN_COMMAND_GAP_MS. Never for a
  // stop: it is the brake the clearance watchdog reaches for, and a brake that
  // waits its turn is not a brake. It bypasses this path entirely while the
  // link is up, but it can still arrive here when one has to be made first.
  if (this->queue_.front().command != Command::STOP && this->last_command_finished_ != 0 &&
      now - this->last_command_finished_ < MIN_COMMAND_GAP_MS)
    return;

  const PendingCommand command = this->queue_.front();
  this->queue_.erase(this->queue_.begin());

  const int distance = std::abs(static_cast<int>(command.target) - static_cast<int>(this->raw_position_));

  if (!this->write_command_(command.command, command.argument)) {
    this->fail_("could not write command");
    return;
  }

  this->in_flight_ = command;
  this->command_in_flight_ = true;
  this->command_sent_at_ = now;
  this->settle_matches_ = 0;
  this->settle_rechecked_ = false;
  this->recheck_attempts_ = 0;
  this->last_activity_ = now;

  // A rail already standing on the requested position has nothing to report,
  // so waiting for it to announce an arrival would always time out. Send the
  // command anyway — the remembered position may be wrong — but do not hold the
  // operation open waiting for movement that was never asked for.
  if (command.command == Command::PERCENT && this->has_position_ && this->raw_position_ == command.target) {
    ESP_LOGD(TAG, "[%s] Already at %u, not waiting for it to move", this->label_,
             static_cast<unsigned>(command.target));
    this->in_flight_.verification = Verification::ACKED;
  }

  // Fixed here, not recomputed each loop: a budget derived from the live
  // position shrinks as the rail approaches, and would cut a long move short
  // long before its real allowance ran out.
  // From the verification actually in force, which the branch above may just
  // have downgraded -- not from the one the command was queued with.
  this->command_budget_ = this->in_flight_.verification == Verification::SETTLED
                              ? TRAVEL_TIMEOUT_BASE_MS + distance * TRAVEL_TIMEOUT_PER_PERCENT_MS
                              : COMMAND_ACK_TIMEOUT_MS;

  if (command.command == Command::PERCENT) {
    this->moving_ = true;
    this->travel_direction_ = command.target == this->raw_position_
                                  ? 0
                                  : (this->range_.to_window(static_cast<float>(command.target)) >
                                             this->range_.to_window(static_cast<float>(this->raw_position_))
                                         ? 1
                                         : -1);
  }

  // Say that a move has started. On a motor that was already connected no state
  // transition follows, so without this the covers would go on reporting idle
  // until the first frame came back -- and a silent motor would look idle for
  // the whole travel budget.
  this->publish_();
}

bool MotionblindsBLEMotor::write_command_(Command command, uint8_t argument) {
  if (this->command_handle_ == 0 || this->ble_client_ == nullptr)
    return false;

  if (this->time_ == nullptr) {
    ESP_LOGE(TAG, "[%s] Refusing command: no time source configured", this->label_);
    return false;
  }
  const ESPTime now = this->time_->now();
  if (!now.is_valid()) {
    ESP_LOGE(TAG, "[%s] Refusing command: clock not synchronised", this->label_);
    return false;
  }

  uint8_t plain[MAX_COMMAND_PREFIX + MotionCrypt::TIMESTAMP_SIZE];
  const size_t body = build_command_body(command, argument, plain);
  if (body == 0)
    return false;

  MotionCrypt::build_timestamp(static_cast<uint8_t>(now.year % 100), now.month, now.day_of_month, now.hour, now.minute,
                              now.second, static_cast<uint16_t>(millis() % 1000), plain + body);

  uint8_t frame[MAX_COMMAND_FRAME];
  const size_t frame_len = MotionCrypt::encrypt(plain, body + MotionCrypt::TIMESTAMP_SIZE, frame, sizeof(frame));
  if (frame_len == 0)
    return false;

  // Written without a response, exactly as the vendor library does; asking for
  // one provokes an ATT "unlikely error" on these motors.
  const esp_err_t status = esp_ble_gattc_write_char(this->ble_client_->get_gattc_if(), this->ble_client_->get_conn_id(),
                                                    this->command_handle_, static_cast<uint16_t>(frame_len), frame,
                                                    ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Write failed: %d", this->label_, status);
    return false;
  }
  return true;
}

// ------------------------------------------------------------ GATT events

void MotionblindsBLEMotor::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      // The link itself is up. Everything before this is the radio finding and
      // reaching the motor; everything after is GATT.
      if (param->open.status == ESP_GATT_OK)
        this->phase_open_at_ = millis();
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->phase_services_at_ = millis();
      // Handles must be re-read on every connection; they are not guaranteed
      // to survive a reconnect, and the service objects are freed shortly.
      auto *command = this->ble_client_->get_characteristic(espbt::ESPBTUUID::from_raw(SERVICE_UUID),
                                                         espbt::ESPBTUUID::from_raw(COMMAND_CHAR_UUID));
      auto *notify = this->ble_client_->get_characteristic(espbt::ESPBTUUID::from_raw(SERVICE_UUID),
                                                        espbt::ESPBTUUID::from_raw(NOTIFICATION_CHAR_UUID));
      if (command == nullptr || notify == nullptr) {
        this->fail_("motor does not expose the Motionblinds service");
        return;
      }
      this->command_handle_ = command->handle;
      this->notify_handle_ = notify->handle;

      auto *descriptor = this->ble_client_->get_config_descriptor(notify->handle);
      this->config_descriptor_handle_ = descriptor == nullptr ? 0 : descriptor->handle;

      this->set_state_(MotorState::HANDSHAKE);
      this->handshake_ = Handshake::WAIT_NOTIFY_REGISTRATION;

      const esp_err_t status = esp_ble_gattc_register_for_notify(this->ble_client_->get_gattc_if(),
                                                                 this->ble_client_->get_remote_bda(), notify->handle);
      if (status != ESP_OK)
        this->fail_("could not register for notifications");
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (this->handshake_ != Handshake::WAIT_NOTIFY_REGISTRATION)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK || param->reg_for_notify.handle != this->notify_handle_) {
        this->fail_("notification registration rejected");
        break;
      }
      // Registering is not the same as having notifications enabled: the
      // client writes the configuration descriptor while handling this event,
      // and only its completion means the motor will actually talk to us.
      this->handshake_ = Handshake::WAIT_DESCRIPTOR_WRITE;
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (this->handshake_ != Handshake::WAIT_DESCRIPTOR_WRITE)
        break;
      if (this->config_descriptor_handle_ != 0 && param->write.handle != this->config_descriptor_handle_)
        break;  // some other descriptor
      if (param->write.status != ESP_GATT_OK) {
        this->fail_("enabling notifications was rejected");
        break;
      }

      // The motor ignores everything until it has been keyed.
      if (!this->write_command_(Command::SET_KEY, 0)) {
        this->fail_("could not send the key");
        break;
      }
      this->handshake_ = Handshake::WAIT_BLIND_SETTLE;
      this->settle_since_ = millis();
      this->phase_notify_at_ = this->settle_since_;
      this->handshake_attempts_ = 0;
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      this->handle_notification_(param->notify.value, param->notify.value_len);
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.handle != this->command_handle_)
        break;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] Command write reported status %d", this->label_, param->write.status);
        break;
      }
      // Local stack completion only. It is not an acknowledgement from the
      // motor and says nothing about whether the blind moved.
      if (this->command_in_flight_ && this->in_flight_.verification == Verification::ACKED) {
        this->command_in_flight_ = false;
        this->finish_operation_();
      }
      break;
    }

    default:
      break;
  }
}

void MotionblindsBLEMotor::handle_notification_(const uint8_t *data, uint16_t length) {
  uint8_t plain[64];
  const size_t plain_len = MotionCrypt::decrypt(data, length, plain, sizeof(plain));
  if (plain_len == 0) {
    ESP_LOGW(TAG, "[%s] Discarding undecryptable notification of %u bytes", this->label_, static_cast<unsigned>(length));
    return;
  }

  Notification notification;
  if (!parse_notification(plain, plain_len, notification)) {
    ESP_LOGD(TAG, "[%s] Ignoring unrecognised notification", this->label_);
    return;
  }
  this->apply_notification_(notification);
}

void MotionblindsBLEMotor::apply_notification_(const Notification &notification) {
  const uint8_t previous = this->raw_position_;
  // A motor reporting for the first time is worth storing even when the value
  // it reports happens to equal the zero it was initialised with, which is
  // exactly the case for a rail parked at the top of its travel.
  const bool first_reading = !this->has_position_;

  this->raw_position_ = notification.position;
  this->raw_tilt_ = notification.tilt;
  this->end_positions_ = notification.end_positions;
  this->has_end_positions_ = true;
  this->has_position_ = true;
  this->position_fresh_ = true;

  if (notification.has_battery) {
    const bool changed = !this->has_battery_ || this->battery_percentage_ != notification.battery_percentage ||
                         this->battery_charging_ != notification.charging;
    this->has_battery_ = true;
    this->battery_percentage_ = notification.battery_percentage;
    this->battery_charging_ = notification.charging;
    if (changed)
      this->publish_();
  }
  if (notification.has_speed) {
    this->has_speed_ = true;
    this->speed_ = notification.speed;
  }
  if (notification.has_favorite)
    this->favorite_set_ = notification.favorite_set;

  if (notification.type == NotificationType::STATUS) {
    this->status_seen_ = true;
    this->ever_status_ = true;
    this->last_status_at_ = millis();
  }

  if (this->handshake_ == Handshake::WAIT_STATUS && notification.type == NotificationType::STATUS) {
    this->handshake_ = Handshake::DONE;
    this->set_state_(MotorState::READY);
    this->attempts_ = 0;
    this->log_connect_phases_();
    ESP_LOGI(TAG, "[%s] Ready, position %u, battery %u%%", this->label_, static_cast<unsigned>(this->raw_position_),
             static_cast<unsigned>(this->battery_percentage_));
  }

  if (this->command_in_flight_) {
    if (this->in_flight_.verification == Verification::STARTED) {
      this->command_in_flight_ = false;
      this->finish_operation_();
    } else if (this->in_flight_.verification == Verification::SETTLED) {
      if (this->raw_position_ == this->in_flight_.target) {
        // Two frames are normally required so a position the rail is merely
        // passing through cannot be mistaken for arrival. After the recheck
        // below that reasoning does not apply: the rail has stood still for the
        // whole travel budget, and this frame is the answer to a question that
        // was asked explicitly. Refusing it made a rail that was already at its
        // commanded position fail every single time -- the recheck asked where
        // the motor was, was told, and condemned it anyway.
        if (++this->settle_matches_ >= SETTLE_FRAMES || notification.type == NotificationType::FEEDBACK ||
            this->settle_rechecked_) {
          ESP_LOGI(TAG, "[%s] Reached %u", this->label_, static_cast<unsigned>(this->raw_position_));
          this->command_in_flight_ = false;
          this->moving_ = false;
          this->travel_direction_ = 0;
          this->finish_operation_();
          this->save_position_();
        }
      } else {
        this->settle_matches_ = 0;
        this->moving_ = true;
      }
    }
  } else if (first_reading || previous != this->raw_position_) {
    // Deliberately not written to flash here. A rail driven by the remote emits
    // a frame for every percent it travels, and committing each one turned what
    // the comment on save_position_() calls "a handful of writes a day" into
    // thousands. The disconnect below writes whatever was learned, once.
    this->publish_();
  }

  this->last_activity_ = millis();
  this->publish_();
}

// ------------------------------------------------------------------ state

void MotionblindsBLEMotor::set_state_(MotorState state) {
  if (this->state_ == state)
    return;
  this->state_ = state;
  this->state_since_ = millis();
  if (state == MotorState::DISCOVERING) {
    this->phase_work_at_ = this->state_since_;
    this->phase_heard_at_ = 0;
    this->phase_open_at_ = 0;
    this->phase_services_at_ = 0;
    this->phase_notify_at_ = 0;
  } else if (state == MotorState::CONNECTING) {
    this->phase_heard_at_ = this->state_since_;
  }
  if (state != MotorState::CONNECTING)
    this->stuck_reported_ = false;
  if (state == MotorState::IDLE || state == MotorState::FAILED)
    this->handshake_ = Handshake::NONE;
  this->publish_();
}

void MotionblindsBLEMotor::fail_(const char *reason) {
  ESP_LOGE(TAG, "[%s] Command failed: %s", this->label_, reason);
  this->queue_.clear();
  // Cleared before anything is published, not after. Both calls below notify
  // the coordinator, which may queue fresh work from inside them, and
  // start_operation_() adopts whatever start time it finds -- so clearing
  // afterwards handed the new operation this failed one's clock.
  this->operation_since_ = 0;
  this->lease_count_ = 0;
  this->set_state_(MotorState::FAILED);
  this->abort_();
}

void MotionblindsBLEMotor::retry_connection_(const char *reason) {
  ESP_LOGW(TAG, "[%s] Connection was %s, dropping it to try a fresh one", this->label_, reason);
  this->command_in_flight_ = false;
  this->command_handle_ = 0;
  this->notify_handle_ = 0;
  this->config_descriptor_handle_ = 0;
  this->handshake_ = Handshake::NONE;
  // The queue is deliberately left intact and the state left in HANDSHAKE, so
  // that reconcile_state_() sees the link go idle and retries with the work
  // still to do, counting against the same attempt limit as any other failure.
  if (this->ble_client_ != nullptr)
    this->ble_client_->set_enabled(false);
}

void MotionblindsBLEMotor::abort_() {
  this->command_in_flight_ = false;
  this->moving_ = false;
  this->travel_direction_ = 0;
  this->settle_matches_ = 0;
  this->command_handle_ = 0;
  this->notify_handle_ = 0;
  this->config_descriptor_handle_ = 0;
  this->handshake_ = Handshake::NONE;
  this->status_seen_ = false;
  this->status_backfilled_ = false;
  this->mark_stale_();

  if (this->ble_client_ != nullptr) {
    this->ble_client_->set_enabled(false);
    if (this->state_ != MotorState::FAILED && this->state_ != MotorState::IDLE)
      this->set_state_(MotorState::DISCONNECTING);
  }
}

void MotionblindsBLEMotor::on_disconnect_complete(esp_err_t reason) {
  ESP_LOGD(TAG, "[%s] Link torn down (reason %d)", this->label_, reason);
  this->command_in_flight_ = false;
  this->moving_ = false;
  this->command_handle_ = 0;
  this->notify_handle_ = 0;
  this->config_descriptor_handle_ = 0;
  this->handshake_ = Handshake::NONE;
  this->connecting_since_ = 0;
  this->mark_stale_();
}

void MotionblindsBLEMotor::mark_stale_() {
  if (this->position_fresh_) {
    this->position_fresh_ = false;
    this->save_position_();
    this->publish_();
  }
}

void MotionblindsBLEMotor::save_position_() {
  PersistedState stored{this->raw_position_,   this->raw_tilt_,   this->battery_percentage_,
                        this->has_position_,   this->has_battery_, this->battery_charging_};
  this->state_pref_.save(&stored);
  // Committed straight away rather than left in the pending buffer. ESPHome
  // only flushes preferences on a clean shutdown, so without this every reset
  // that does not get that far -- a crash, a power cut, some reflashes -- throws
  // the position away and the blind comes back not knowing where its rails
  // are. Saves happen when a move completes and when the link is dropped, so
  // this really is a handful of writes a day.
  //
  // The result is checked. A store that has stopped accepting writes is
  // otherwise completely silent: everything keeps working until the next
  // restart, when every rail comes back not knowing where it is.
  if (!global_preferences->sync()) {
    if (!this->save_failed_) {
      ESP_LOGE(TAG, "[%s] Flash store refused the write; nothing will survive a restart", this->label_);
      this->save_failed_ = true;
    }
    return;
  }
  this->save_failed_ = false;

  // Saves are rare now -- a completed move, and dropping the link -- so saying
  // what went in costs nothing and closes the loop with the line above. A save
  // that carries no position is worth a warning: it is how a working store ends
  // up holding a rail that does not know where it is.
  if (this->has_position_) {
    if (this->has_battery_) {
      ESP_LOGI(TAG, "[%s] Saved position %u, battery %u%%", this->label_, static_cast<unsigned>(this->raw_position_),
               static_cast<unsigned>(this->battery_percentage_));
    } else {
      ESP_LOGI(TAG, "[%s] Saved position %u, no battery reading", this->label_,
               static_cast<unsigned>(this->raw_position_));
    }
  } else {
    ESP_LOGW(TAG, "[%s] Saved without a position; a restart will not know where this rail is", this->label_);
  }
}

void MotionblindsBLEMotor::publish_() { this->update_callback_.call(); }

float MotionblindsBLEMotor::window_position() const {
  if (!this->has_position_)
    return NAN;
  return this->range_.to_window(static_cast<float>(this->raw_position_));
}

optional<uint8_t> MotionblindsBLEMotor::battery_percentage() const {
  if (!this->has_battery_)
    return {};
  return this->battery_percentage_;
}

optional<bool> MotionblindsBLEMotor::battery_charging() const {
  if (!this->has_battery_)
    return {};
  return this->battery_charging_;
}

optional<SpeedLevel> MotionblindsBLEMotor::speed() const {
  if (!this->has_speed_)
    return {};
  return this->speed_;
}

optional<int8_t> MotionblindsBLEMotor::signal_strength() const {
  if (!this->has_signal_)
    return {};
  return this->signal_strength_;
}

void MotionblindsBLEMotor::set_signal_strength(int8_t rssi) {
  this->has_signal_ = true;
  this->signal_strength_ = rssi;
  this->publish_();
}

}  // namespace esphome::motionblinds_ble

#endif  // USE_ESP32
