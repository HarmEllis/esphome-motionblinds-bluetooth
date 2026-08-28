#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::motionblinds_ble {

/* Frame encryption for Motionblinds Bluetooth.
 *
 * Every frame is AES-128-ECB with a fixed key shared by all motors, PKCS#7
 * padded. Commands carry a wall-clock timestamp that the library regenerates
 * immediately before each write; whether the motor validates it or merely
 * treats it as a nonce is not established by any available source.
 *
 * Deliberately free of ESPHome and Bluetooth dependencies so the host tests
 * exercise exactly the code that runs on the ESP32.
 */
class MotionCrypt {
 public:
  static constexpr size_t BLOCK_SIZE = 16;
  static constexpr size_t TIMESTAMP_SIZE = 8;

  /// Encrypt with PKCS#7 padding. Returns the ciphertext length, or 0 if it
  /// would not fit in out_capacity.
  static size_t encrypt(const uint8_t *plain, size_t plain_len, uint8_t *out, size_t out_capacity);

  /// Decrypt and strip PKCS#7. Returns the plaintext length, or 0 when the
  /// input length, the padding, or the capacity is invalid. Callers must treat
  /// 0 as "reject this frame" and never index into out.
  static size_t decrypt(const uint8_t *cipher, size_t cipher_len, uint8_t *out, size_t out_capacity);

  /// Serialise a wall-clock instant into the 8 trailing timestamp bytes.
  /// Each field is its plain numeric value, not BCD; the millisecond is
  /// big-endian across the final two bytes.
  static void build_timestamp(uint8_t year_mod_100, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                              uint8_t second, uint16_t millisecond, uint8_t out[TIMESTAMP_SIZE]);
};

}  // namespace esphome::motionblinds_ble
