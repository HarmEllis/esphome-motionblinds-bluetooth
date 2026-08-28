#pragma once

#include <cstdint>

namespace esphome::motionblinds_ble {

/* Minimal AES-128 ECB block cipher.
 *
 * Vendored rather than taken from mbedtls so that the exact same code runs on
 * the ESP32 and in the host unit tests. Motionblinds frames are a single
 * 16-byte block each, so the throughput of a table-free implementation is
 * irrelevant here.
 */
class Aes128 {
 public:
  /// Expand a 16-byte key into the round keys used by encrypt/decrypt.
  explicit Aes128(const uint8_t key[16]);

  void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
  void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;

 private:
  uint8_t round_keys_[176];
};

}  // namespace esphome::motionblinds_ble
