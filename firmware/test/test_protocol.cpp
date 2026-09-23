#include <unity.h>

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

void runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_standard_vector);
  RUN_TEST(test_little_endian_helpers);
  RUN_TEST(test_upload_progress_pixels);
  RUN_TEST(test_protocol_v2_playlist_metadata_integrity);
  RUN_TEST(test_wifi_command_bytes_do_not_collide);
  RUN_TEST(test_hotspot_validation_and_fallback_policy);
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
