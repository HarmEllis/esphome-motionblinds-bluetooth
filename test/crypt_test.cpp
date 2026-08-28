// Host test for the Motionblinds frame format.
//
// Build and run:
//   g++ -std=c++17 -O2 -Wall -Wextra -I esphome/components/motionblinds_ble
//       test/crypt_test.cpp
//       esphome/components/motionblinds_ble/motionblinds_aes.cpp
//       esphome/components/motionblinds_ble/motionblinds_crypt.cpp
//       -o crypt_test && ./crypt_test
//
// The golden vectors were produced with the reference Python implementation
// (motionblindsble 0.1.3), so a passing run means our AES, our padding, our
// timestamp layout and our command bodies agree with the library that is known
// to drive these motors.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "motionblinds_protocol.h"

using namespace esphome::motionblinds_ble;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &what) {
  g_checks++;
  if (!condition) {
    g_failures++;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

std::vector<uint8_t> from_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

std::string to_hex(const uint8_t *data, size_t len) {
  static const char *DIGITS = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out.push_back(DIGITS[data[i] >> 4]);
    out.push_back(DIGITS[data[i] & 0x0F]);
  }
  return out;
}

// year 26, month 8, day 28, 09:05:42.500
void reference_timestamp(uint8_t out[MotionCrypt::TIMESTAMP_SIZE]) {
  MotionCrypt::build_timestamp(26, 8, 28, 9, 5, 42, 500, out);
}

struct CommandVector {
  const char *name;
  Command command;
  uint8_t argument;
  const char *expected_body_hex;   // body only, without the timestamp
  const char *expected_cipher_hex;
};

// Straight from the reference implementation; see the header comment.
const CommandVector COMMAND_VECTORS[] = {
    {"SET_KEY", Command::SET_KEY, 0, "02c001", "ca8a9be738fefa420f0713daedc75f67"},
    {"STATUS_QUERY", Command::STATUS_QUERY, 0, "03050f02", "f6863f6caa4ebb38b4c9e08ae686a676"},
    {"OPEN", Command::OPEN, 0, "03020301", "e525fcab63ac955f85b0dce1679fb285"},
    {"CLOSE", Command::CLOSE, 0, "03020302", "e9f34a37f1c405d0aed7404e0723e405"},
    {"STOP", Command::STOP, 0, "03020303", "97a6ed24500f2c396120375a07a4278d"},
    {"FAVORITE", Command::FAVORITE, 0, "03020306", "b7d4ec53eae53f0f2c5f50958213e7cb"},
    {"PERCENT 50", Command::PERCENT, 50, "050204403200", "e79acc4647cc14f3252e499564ac2226"},
    {"PERCENT 0", Command::PERCENT, 0, "050204400000", "0fca715b704ffa05a9d95fd7412def2f"},
    {"PERCENT 100", Command::PERCENT, 100, "050204406400", "bf6e692c4fb251ecd9204829afebc9e7"},
    {"SPEED high", Command::SPEED, 3, "0403010a03", "4821e9b10691f301c08ef55022a3ffdb"},
    {"ANGLE 180", Command::ANGLE, 180, "0502042000b4", "4d177db66be19b7100d760834fddec99"},
};

const char *STATUS_PLAIN_HEX = "12040f020c002800000000000200640000b5";
const char *STATUS_CIPHER_HEX = "02b894a4fde83324316ac133929238548e79bccbd4b071acf6fed44eb9dce76e";
const char *FEEDBACK_PLAIN_HEX = "070404020c005a00";
const char *FEEDBACK_CIPHER_HEX = "59e6337b27cdb44853f24f452b9cf512";

void test_commands() {
  std::printf("commands\n");
  uint8_t timestamp[MotionCrypt::TIMESTAMP_SIZE];
  reference_timestamp(timestamp);

  check(to_hex(timestamp, sizeof(timestamp)) == "1a081c09052a01f4", "timestamp layout");

  for (const auto &vector : COMMAND_VECTORS) {
    uint8_t plain[MAX_COMMAND_PREFIX + MotionCrypt::TIMESTAMP_SIZE];
    const size_t body_len = build_command_body(vector.command, vector.argument, plain);
    check(to_hex(plain, body_len) == vector.expected_body_hex, std::string(vector.name) + " body");

    std::memcpy(plain + body_len, timestamp, sizeof(timestamp));
    const size_t plain_len = body_len + sizeof(timestamp);

    uint8_t cipher[MAX_COMMAND_FRAME];
    const size_t cipher_len = MotionCrypt::encrypt(plain, plain_len, cipher, sizeof(cipher));

    check(cipher_len == MotionCrypt::BLOCK_SIZE, std::string(vector.name) + " is exactly one block");
    check(to_hex(cipher, cipher_len) == vector.expected_cipher_hex, std::string(vector.name) + " ciphertext");

    // And back again.
    uint8_t round_trip[MAX_COMMAND_FRAME];
    const size_t round_trip_len = MotionCrypt::decrypt(cipher, cipher_len, round_trip, sizeof(round_trip));
    check(round_trip_len == plain_len && std::memcmp(round_trip, plain, plain_len) == 0,
          std::string(vector.name) + " round trip");
  }
}

void test_status_notification() {
  std::printf("status notification\n");
  const auto cipher = from_hex(STATUS_CIPHER_HEX);

  uint8_t plain[64];
  const size_t plain_len = MotionCrypt::decrypt(cipher.data(), cipher.size(), plain, sizeof(plain));
  check(plain_len == 18, "STATUS decrypts to 18 bytes");
  check(to_hex(plain, plain_len) == STATUS_PLAIN_HEX, "STATUS plaintext");
  check(cipher.size() == 2 * MotionCrypt::BLOCK_SIZE, "STATUS is two blocks, unlike every command");

  Notification notification;
  check(parse_notification(plain, plain_len, notification), "STATUS parses");
  check(notification.type == NotificationType::STATUS, "STATUS type");
  check(notification.position == 40, "STATUS position");
  check(notification.tilt == 0, "STATUS tilt");
  check(notification.end_positions == EndPositions::BOTH, "STATUS end positions");
  check(notification.has_speed && notification.speed == SpeedLevel::MEDIUM, "STATUS speed");
  check(notification.has_favorite && notification.favorite_set, "STATUS favorite set");
  check(notification.has_battery && notification.battery_percentage == 53, "STATUS battery percentage");
  check(notification.charging, "STATUS charging flag");
  check(!notification.wired, "STATUS not wired");
}

void test_feedback_notification() {
  std::printf("feedback notification\n");
  const auto cipher = from_hex(FEEDBACK_CIPHER_HEX);

  uint8_t plain[64];
  const size_t plain_len = MotionCrypt::decrypt(cipher.data(), cipher.size(), plain, sizeof(plain));
  check(to_hex(plain, plain_len) == FEEDBACK_PLAIN_HEX, "FEEDBACK plaintext");

  Notification notification;
  check(parse_notification(plain, plain_len, notification), "FEEDBACK parses");
  check(notification.type == NotificationType::FEEDBACK, "FEEDBACK type");
  check(notification.position == 90, "FEEDBACK position");
  check(notification.end_positions == EndPositions::BOTH, "FEEDBACK end positions");
  check(!notification.has_battery, "FEEDBACK carries no battery");
  check(!notification.has_speed, "FEEDBACK carries no speed");
}

void test_end_positions() {
  std::printf("end positions\n");
  check(end_positions_from_byte(0x00) == EndPositions::NONE, "0x00 -> NONE");
  check(end_positions_from_byte(0x04) == EndPositions::ONE, "0x04 -> ONE");
  check(end_positions_from_byte(0x08) == EndPositions::ONE, "0x08 -> ONE");
  check(end_positions_from_byte(0x0C) == EndPositions::BOTH, "0x0C -> BOTH");
  // The high nibble is not part of the field.
  check(end_positions_from_byte(0xFC) == EndPositions::BOTH, "high nibble ignored");
}

// A malformed frame must be rejected before anything indexes into it.
void test_rejects_malformed_input() {
  std::printf("malformed input\n");
  uint8_t out[64];

  const auto valid = from_hex(FEEDBACK_CIPHER_HEX);
  check(MotionCrypt::decrypt(valid.data(), 0, out, sizeof(out)) == 0, "empty ciphertext rejected");
  check(MotionCrypt::decrypt(valid.data(), 15, out, sizeof(out)) == 0, "non-block-multiple rejected");
  check(MotionCrypt::decrypt(valid.data(), valid.size(), out, 8) == 0, "insufficient capacity rejected");

  // Corrupting the last block destroys the padding, which must not be
  // mistaken for a short but valid plaintext.
  auto corrupted = valid;
  corrupted[corrupted.size() - 1] ^= 0xFF;
  check(MotionCrypt::decrypt(corrupted.data(), corrupted.size(), out, sizeof(out)) == 0, "bad padding rejected");

  Notification notification;
  const uint8_t unknown[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0};
  check(!parse_notification(unknown, sizeof(unknown), notification), "unknown notification type rejected");

  // A STATUS prefix with a FEEDBACK-sized body must not be read as STATUS.
  const uint8_t truncated_status[8] = {0x12, 0x04, 0x0f, 0x02, 0x0C, 0x00, 0x28, 0x00};
  check(!parse_notification(truncated_status, sizeof(truncated_status), notification), "truncated STATUS rejected");

  const uint8_t truncated_feedback[7] = {0x07, 0x04, 0x04, 0x02, 0x0C, 0x00, 0x5A};
  check(!parse_notification(truncated_feedback, sizeof(truncated_feedback), notification),
        "truncated FEEDBACK rejected");

  check(!parse_notification(nullptr, 0, notification), "empty notification rejected");
}

void test_padding_edge_case() {
  std::printf("padding\n");
  // PKCS#7 on an exact multiple of the block size adds a whole extra block.
  uint8_t plain[16];
  std::memset(plain, 0x41, sizeof(plain));
  uint8_t cipher[64];
  const size_t cipher_len = MotionCrypt::encrypt(plain, sizeof(plain), cipher, sizeof(cipher));
  check(cipher_len == 32, "exact block multiple grows by one block");

  uint8_t round_trip[64];
  const size_t round_trip_len = MotionCrypt::decrypt(cipher, cipher_len, round_trip, sizeof(round_trip));
  check(round_trip_len == 16 && std::memcmp(round_trip, plain, 16) == 0, "padding round trip");

  uint8_t too_small[8];
  check(MotionCrypt::encrypt(plain, sizeof(plain), too_small, sizeof(too_small)) == 0, "encrypt respects capacity");
}

}  // namespace

int main() {
  test_commands();
  test_status_notification();
  test_feedback_notification();
  test_end_positions();
  test_rejects_malformed_input();
  test_padding_edge_case();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
