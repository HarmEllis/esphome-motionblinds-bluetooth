#include "motionblinds_crypt.h"

#include <cstring>

#include "motionblinds_aes.h"

namespace esphome::motionblinds_ble {

namespace {

// Shared by every Motionblinds motor; it is not a per-device secret.
const uint8_t ENCRYPTION_KEY[16] = {'a', '3', 'q', '8', 'r', '8', 'c', '1', '3', '5', 's', 'q', 'b', 'n', '6', '6'};

const Aes128 &cipher() {
  static const Aes128 instance{ENCRYPTION_KEY};
  return instance;
}

}  // namespace

size_t MotionCrypt::encrypt(const uint8_t *plain, size_t plain_len, uint8_t *out, size_t out_capacity) {
  // PKCS#7 always appends padding, so an exact multiple of the block size
  // grows by a whole extra block.
  const size_t pad = BLOCK_SIZE - (plain_len % BLOCK_SIZE);
  const size_t total = plain_len + pad;
  if (total > out_capacity)
    return 0;

  uint8_t padded[64];
  if (total > sizeof(padded))
    return 0;
  memcpy(padded, plain, plain_len);
  memset(padded + plain_len, static_cast<int>(pad), pad);

  for (size_t offset = 0; offset < total; offset += BLOCK_SIZE)
    cipher().encrypt_block(padded + offset, out + offset);

  return total;
}

size_t MotionCrypt::decrypt(const uint8_t *cipher_text, size_t cipher_len, uint8_t *out, size_t out_capacity) {
  if (cipher_len == 0 || cipher_len % BLOCK_SIZE != 0 || cipher_len > out_capacity)
    return 0;

  for (size_t offset = 0; offset < cipher_len; offset += BLOCK_SIZE)
    cipher().decrypt_block(cipher_text + offset, out + offset);

  const uint8_t pad = out[cipher_len - 1];
  if (pad == 0 || pad > BLOCK_SIZE || pad > cipher_len)
    return 0;
  for (size_t i = cipher_len - pad; i < cipher_len; i++) {
    if (out[i] != pad)
      return 0;
  }

  return cipher_len - pad;
}

void MotionCrypt::build_timestamp(uint8_t year_mod_100, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                  uint8_t second, uint16_t millisecond, uint8_t out[TIMESTAMP_SIZE]) {
  out[0] = year_mod_100;
  out[1] = month;
  out[2] = day;
  out[3] = hour;
  out[4] = minute;
  out[5] = second;
  out[6] = static_cast<uint8_t>(millisecond >> 8);
  out[7] = static_cast<uint8_t>(millisecond & 0xFF);
}

}  // namespace esphome::motionblinds_ble
