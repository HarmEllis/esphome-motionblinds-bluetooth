#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "motionblinds_crypt.h"

namespace esphome::motionblinds_ble {

/* Wire format of the Motionblinds Bluetooth protocol.
 *
 * Header-only and dependency-free so that both the ESP32 component and the
 * host tests share one definition of the format. Everything here works on raw
 * plaintext; encryption is the caller's job.
 *
 * Raw motor positions follow the protocol convention: 0 is fully open,
 * 100 is fully closed. Translating that into window coordinates is done by the
 * motor component, never here.
 */

/// 128-bit UUIDs, in the byte order used by the specification (most
/// significant byte first). ESPBTUUID::from_raw() expects the reverse.
static constexpr const char *SERVICE_UUID = "d973f2e0-b19e-11e2-9e96-0800200c9a66";
static constexpr const char *NOTIFICATION_CHAR_UUID = "d973f2e1-b19e-11e2-9e96-0800200c9a66";
static constexpr const char *COMMAND_CHAR_UUID = "d973f2e2-b19e-11e2-9e96-0800200c9a66";

enum class Command : uint8_t {
  OPEN,
  CLOSE,
  STOP,
  OPEN_TILT,
  CLOSE_TILT,
  FAVORITE,
  PERCENT,
  ANGLE,
  SPEED,
  SET_KEY,
  STATUS_QUERY,
  USER_QUERY,
  POINT_SET_QUERY,
};

enum class SpeedLevel : uint8_t {
  LOW = 1,
  MEDIUM = 2,
  HIGH = 3,
};

/// How many of the two end positions the motor has been calibrated for. A
/// motor reporting anything but BOTH cannot be driven to a position.
enum class EndPositions : uint8_t {
  NONE = 0,
  ONE = 1,
  BOTH = 2,
};

enum class NotificationType : uint8_t {
  UNKNOWN,
  FEEDBACK,
  STATUS,
};

/// Longest command body before the timestamp is appended.
static constexpr size_t MAX_COMMAND_PREFIX = 6;
/// Every command therefore fits in a single padded AES block. Incoming STATUS
/// frames do not; they are two blocks.
static constexpr size_t MAX_COMMAND_FRAME = MotionCrypt::BLOCK_SIZE * 2;

/// Decoded contents of one notification. Fields are only meaningful when the
/// corresponding has_* flag is set.
struct Notification {
  NotificationType type{NotificationType::UNKNOWN};
  uint8_t position{0};  ///< raw protocol position, 0 = open, 100 = closed
  uint8_t tilt{0};
  EndPositions end_positions{EndPositions::NONE};

  bool has_battery{false};
  uint8_t battery_percentage{0};
  bool charging{false};
  bool wired{false};

  bool has_speed{false};
  SpeedLevel speed{SpeedLevel::MEDIUM};

  bool has_favorite{false};
  bool favorite_set{false};
};

/// Build the plaintext body of a command, excluding the timestamp.
/// `argument` is the position for PERCENT, the angle for ANGLE and the level
/// for SPEED; it is ignored otherwise. Returns the number of bytes written.
inline size_t build_command_body(Command command, uint8_t argument, uint8_t *out) {
  switch (command) {
    case Command::OPEN:
      memcpy(out, "\x03\x02\x03\x01", 4);
      return 4;
    case Command::CLOSE:
      memcpy(out, "\x03\x02\x03\x02", 4);
      return 4;
    case Command::STOP:
      memcpy(out, "\x03\x02\x03\x03", 4);
      return 4;
    case Command::OPEN_TILT:
      memcpy(out, "\x03\x02\x03\x09", 4);
      return 4;
    case Command::CLOSE_TILT:
      memcpy(out, "\x03\x02\x03\x0a", 4);
      return 4;
    case Command::FAVORITE:
      memcpy(out, "\x03\x02\x03\x06", 4);
      return 4;
    case Command::PERCENT:
      memcpy(out, "\x05\x02\x04\x40", 4);
      out[4] = argument;
      out[5] = 0x00;
      return 6;
    case Command::ANGLE:
      memcpy(out, "\x05\x02\x04\x20", 4);
      out[4] = 0x00;
      out[5] = argument;
      return 6;
    case Command::SPEED:
      memcpy(out, "\x04\x03\x01\x0a", 4);
      out[4] = argument;
      return 5;
    case Command::SET_KEY:
      memcpy(out, "\x02\xc0\x01", 3);
      return 3;
    case Command::STATUS_QUERY:
      memcpy(out, "\x03\x05\x0f\x02", 4);
      return 4;
    case Command::USER_QUERY:
      memcpy(out, "\x02\xc0\x05", 3);
      return 3;
    case Command::POINT_SET_QUERY:
      memcpy(out, "\x03\x05\x01\x20", 4);
      return 4;
  }
  return 0;
}

/// Map the end-position nibble onto how many end positions are set.
inline EndPositions end_positions_from_byte(uint8_t value) {
  static const EndPositions MAPPING[4] = {EndPositions::NONE, EndPositions::ONE, EndPositions::ONE,
                                          EndPositions::BOTH};
  return MAPPING[(value & 0x0F) >> 2];
}

/// Parse a decrypted notification. Returns false for anything unrecognised or
/// too short, so a malformed frame can never make the caller index past the
/// end of the buffer.
inline bool parse_notification(const uint8_t *plain, size_t len, Notification &out) {
  out = Notification{};

  static const uint8_t FEEDBACK_PREFIX[4] = {0x07, 0x04, 0x04, 0x02};
  static const uint8_t STATUS_PREFIX[4] = {0x12, 0x04, 0x0f, 0x02};

  if (len >= 8 && memcmp(plain, FEEDBACK_PREFIX, 4) == 0) {
    out.type = NotificationType::FEEDBACK;
    out.end_positions = end_positions_from_byte(plain[4]);
    out.position = plain[6];
    out.tilt = plain[7];
    return true;
  }

  if (len >= 18 && memcmp(plain, STATUS_PREFIX, 4) == 0) {
    out.type = NotificationType::STATUS;
    out.end_positions = end_positions_from_byte(plain[4]);
    out.position = plain[6];
    out.tilt = plain[7];

    const uint8_t speed_byte = plain[12];
    if (speed_byte >= 1 && speed_byte <= 3) {
      out.has_speed = true;
      out.speed = static_cast<SpeedLevel>(speed_byte);
    }

    // Favorite position is a little-endian uint16; the motor reports only
    // whether one exists, never where it is.
    out.has_favorite = true;
    out.favorite_set = (static_cast<uint16_t>(plain[14]) | (static_cast<uint16_t>(plain[15]) << 8)) != 0;

    const uint8_t battery = plain[17];
    out.has_battery = true;
    out.wired = battery == 0xFF;
    out.charging = (battery & 0x80) != 0;
    out.battery_percentage = static_cast<uint8_t>((battery & 0x7F) > 100 ? 100 : (battery & 0x7F));
    return true;
  }

  return false;
}

}  // namespace esphome::motionblinds_ble
