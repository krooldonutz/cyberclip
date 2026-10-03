#include <unity.h>

#include "../ams.h"
#include "../protocol.h"

using namespace cyberclip;

void setUp() {}
void tearDown() {}

void test_crc_standard_vector() {
  const uint8_t input[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16CcittFalse(input, sizeof(input)));
}

void test_little_endian_helpers() {
  uint8_t bytes[4];
  writeLe32(bytes, 0x89ABCDEF);
  TEST_ASSERT_EQUAL_HEX8(0xEF, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0x89, bytes[3]);
  TEST_ASSERT_EQUAL_HEX32(0x89ABCDEF, readLe32(bytes));
  writeLe16(bytes, 0x1234);
  TEST_ASSERT_EQUAL_HEX16(0x1234, readLe16(bytes));
}

void test_upload_progress_pixels() {
  TEST_ASSERT_EQUAL_UINT16(0, uploadProgressPixels(100, 0, 0, 0, 0));
  TEST_ASSERT_EQUAL_UINT16(0, uploadProgressPixels(100, 4, 0, 0, 1000));
  TEST_ASSERT_EQUAL_UINT16(12, uploadProgressPixels(100, 4, 0, 500, 1000));
  TEST_ASSERT_EQUAL_UINT16(37, uploadProgressPixels(100, 4, 1, 500, 1000));
  TEST_ASSERT_EQUAL_UINT16(75, uploadProgressPixels(100, 4, 3, 0, 0));
  TEST_ASSERT_EQUAL_UINT16(100,
                           uploadProgressPixels(100, 4, 4, 0, 1000));
  TEST_ASSERT_EQUAL_UINT16(
      25, uploadProgressPixels(100, 4, 0, UINT32_MAX, UINT32_MAX));
}

void test_protocol_v2_playlist_metadata_integrity() {
  uint8_t metadata[playlistMetadataSize(2)] = {
      'C', 'C', 'P', 'L', kPlaylistMetadataVersion, 1, MEDIA_GIF, 2, 3, 1};
  writeLe16(metadata + kPlaylistMetadataHeaderSize, 20);
  writeLe16(metadata + kPlaylistMetadataHeaderSize + 2, 125);
  writeLe16(metadata + sizeof(metadata) - kPlaylistMetadataCrcSize,
            crc16CcittFalse(metadata,
                            sizeof(metadata) - kPlaylistMetadataCrcSize));

  TEST_ASSERT_EQUAL_UINT8(2, kProtocolVersion);
  TEST_ASSERT_EQUAL_UINT8(20, kHelloResponseFixedSize);
  TEST_ASSERT_EQUAL_HEX8(0x30, BEGIN_PLAYLIST);
  TEST_ASSERT_TRUE(validatePlaylistMetadata(metadata, sizeof(metadata)));
  metadata[11] ^= 1;
  TEST_ASSERT_FALSE(validatePlaylistMetadata(metadata, sizeof(metadata)));
}

void test_wifi_command_bytes_do_not_collide() {
  TEST_ASSERT_EQUAL_HEX8(0x40, SET_WIFI_CREDENTIALS);
  TEST_ASSERT_EQUAL_HEX8(0x41, GET_WIFI_STATUS);
  TEST_ASSERT_EQUAL_HEX8(0x42, CLEAR_WIFI_CREDENTIALS);
  TEST_ASSERT_EQUAL_HEX8(0x43, SET_WIFI_ENABLED);
  TEST_ASSERT_EQUAL_HEX8(0x44, SET_HOTSPOT_CONFIG);
  TEST_ASSERT_EQUAL_HEX8(0x45, GET_HOTSPOT_STATUS);
  TEST_ASSERT_EQUAL_HEX8(0xA3, WIFI_STATUS_RESPONSE);
  TEST_ASSERT_EQUAL_HEX8(0xA4, HOTSPOT_STATUS_RESPONSE);
  TEST_ASSERT_EQUAL_UINT8(16, kWifiTokenSize);
  TEST_ASSERT_EQUAL_UINT8(32, kWifiMaxSsidLength);
  TEST_ASSERT_EQUAL_UINT8(64, kWifiMaxPasswordLength);
}

void test_hotspot_validation_and_fallback_policy() {
  TEST_ASSERT_TRUE(isValidHotspotMode(HOTSPOT_OFF));
  TEST_ASSERT_TRUE(isValidHotspotMode(HOTSPOT_ALWAYS));
  TEST_ASSERT_FALSE(isValidHotspotMode(3));
  TEST_ASSERT_FALSE(isValidHotspotPasswordLength(7));
  TEST_ASSERT_TRUE(isValidHotspotPasswordLength(8));
  TEST_ASSERT_TRUE(isValidHotspotPasswordLength(63));
  TEST_ASSERT_FALSE(isValidHotspotPasswordLength(64));

  TEST_ASSERT_FALSE(shouldRunHotspot(HOTSPOT_OFF, false, false, 20000, 15000));
  TEST_ASSERT_TRUE(shouldRunHotspot(HOTSPOT_ALWAYS, true, true, 0, 15000));
  TEST_ASSERT_TRUE(
      shouldRunHotspot(HOTSPOT_FALLBACK, false, false, 0, 15000));
  TEST_ASSERT_FALSE(
      shouldRunHotspot(HOTSPOT_FALLBACK, true, false, 14999, 15000));
  TEST_ASSERT_TRUE(
      shouldRunHotspot(HOTSPOT_FALLBACK, true, false, 15000, 15000));
  TEST_ASSERT_FALSE(
      shouldRunHotspot(HOTSPOT_FALLBACK, true, true, 20000, 15000));
}

void test_power_policy() {
  TEST_ASSERT_FALSE(onBatteryPower(true, 4350));
  TEST_ASSERT_TRUE(onBatteryPower(false, 4249));
  TEST_ASSERT_TRUE(onBatteryPower(true, 4300));
  TEST_ASSERT_FALSE(onBatteryPower(false, 4300));

  TEST_ASSERT_TRUE(shouldRunAtFullSpeed(false, false, 60000, 3000));
  TEST_ASSERT_TRUE(shouldRunAtFullSpeed(true, true, 60000, 3000));
  TEST_ASSERT_TRUE(shouldRunAtFullSpeed(true, false, 2999, 3000));
  TEST_ASSERT_FALSE(shouldRunAtFullSpeed(true, false, 3000, 3000));

  TEST_ASSERT_FALSE(shouldSuspendWifi(false, false, 600000, 120000));
  TEST_ASSERT_FALSE(shouldSuspendWifi(true, false, 119999, 120000));
  TEST_ASSERT_TRUE(shouldSuspendWifi(true, false, 120000, 120000));
  TEST_ASSERT_FALSE(shouldSuspendWifi(true, true, 600000, 120000));

  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(true, false, 0));
  TEST_ASSERT_EQUAL_UINT32(1, loopIdleSleepMs(false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(false, true, 1));
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(false, true, -5));
  TEST_ASSERT_EQUAL_UINT32(1, loopIdleSleepMs(false, true, 2));
}

void test_battery_percent_and_level() {
  TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMillivolts(0));
  TEST_ASSERT_EQUAL_UINT8(0, batteryPercentFromMillivolts(3300));
  TEST_ASSERT_EQUAL_UINT8(50, batteryPercentFromMillivolts(3800));
  TEST_ASSERT_EQUAL_UINT8(57, batteryPercentFromMillivolts(3850));
  TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMillivolts(4200));
  TEST_ASSERT_EQUAL_UINT8(100, batteryPercentFromMillivolts(5000));

  TEST_ASSERT_EQUAL_UINT8(BATTERY_LOW, batteryLevelFromPercent(0));
  TEST_ASSERT_EQUAL_UINT8(BATTERY_LOW, batteryLevelFromPercent(33));
  TEST_ASSERT_EQUAL_UINT8(BATTERY_MEDIUM, batteryLevelFromPercent(34));
  TEST_ASSERT_EQUAL_UINT8(BATTERY_MEDIUM, batteryLevelFromPercent(66));
  TEST_ASSERT_EQUAL_UINT8(BATTERY_HIGH, batteryLevelFromPercent(67));
  TEST_ASSERT_EQUAL_UINT8(BATTERY_HIGH, batteryLevelFromPercent(100));
}

void test_ams_entity_update_parsing() {
  const uint8_t notification[] = {ams::ENTITY_TRACK, ams::TRACK_TITLE, 0x01,
                                  'S', 'o', 'n', 'g'};
  ams::EntityUpdate update;
  TEST_ASSERT_TRUE(
      ams::parseEntityUpdate(notification, sizeof(notification), &update));
  TEST_ASSERT_EQUAL_UINT8(ams::ENTITY_TRACK, update.entity);
  TEST_ASSERT_EQUAL_UINT8(ams::TRACK_TITLE, update.attribute);
  TEST_ASSERT_TRUE(update.truncated);
  TEST_ASSERT_EQUAL_UINT32(4, update.valueLength);
  TEST_ASSERT_EQUAL_MEMORY("Song", update.value, 4);

  const uint8_t empty[] = {ams::ENTITY_TRACK, ams::TRACK_ARTIST, 0x00};
  TEST_ASSERT_TRUE(ams::parseEntityUpdate(empty, sizeof(empty), &update));
  TEST_ASSERT_FALSE(update.truncated);
  TEST_ASSERT_EQUAL_UINT32(0, update.valueLength);
  TEST_ASSERT_FALSE(ams::parseEntityUpdate(empty, 2, &update));
}

void test_ams_playback_state() {
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_PLAYING,
                          ams::parsePlaybackState("1,1.0,12.5", 10));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_PAUSED,
                          ams::parsePlaybackState("0,0.0,3.1", 9));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_PAUSED, ams::parsePlaybackState("0", 1));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_UNKNOWN, ams::parsePlaybackState("", 0));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_UNKNOWN,
                          ams::parsePlaybackState("12,1.0", 6));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_UNKNOWN,
                          ams::parsePlaybackState("9,1.0", 5));
}

void test_ams_utf8_storage() {
  // "café" is 5 bytes; cutting at 4 would split the 2-byte "é".
  const char cafe[] = "caf\xC3\xA9";
  TEST_ASSERT_EQUAL_UINT32(5, ams::utf8Boundary(cafe, 5, 5));
  TEST_ASSERT_EQUAL_UINT32(3, ams::utf8Boundary(cafe, 5, 4));
  // A 3-byte character ("あ") cut after its first or second byte.
  const char kana[] = "a\xE3\x81\x82";
  TEST_ASSERT_EQUAL_UINT32(1, ams::utf8Boundary(kana, 4, 2));
  TEST_ASSERT_EQUAL_UINT32(1, ams::utf8Boundary(kana, 4, 3));

  char buffer[5] = "";
  TEST_ASSERT_TRUE(ams::storeUtf8(buffer, sizeof(buffer), cafe, 5));
  TEST_ASSERT_EQUAL_STRING("caf", buffer);
  TEST_ASSERT_FALSE(ams::storeUtf8(buffer, sizeof(buffer), cafe, 5));
  TEST_ASSERT_TRUE(ams::storeUtf8(buffer, sizeof(buffer), "", 0));
  TEST_ASSERT_EQUAL_STRING("", buffer);
  TEST_ASSERT_FALSE(ams::storeUtf8(buffer, sizeof(buffer), "", 0));
}

void runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_battery_percent_and_level);
  RUN_TEST(test_crc_standard_vector);
  RUN_TEST(test_little_endian_helpers);
  RUN_TEST(test_upload_progress_pixels);
  RUN_TEST(test_protocol_v2_playlist_metadata_integrity);
  RUN_TEST(test_wifi_command_bytes_do_not_collide);
  RUN_TEST(test_hotspot_validation_and_fallback_policy);
  RUN_TEST(test_power_policy);
  RUN_TEST(test_ams_entity_update_parsing);
  RUN_TEST(test_ams_playback_state);
  RUN_TEST(test_ams_utf8_storage);
  UNITY_END();
}

#ifdef ARDUINO
void setup() {
  runTests();
}

void loop() {}
#else
int main(int, char **) {
  runTests();
  return 0;
}
#endif
