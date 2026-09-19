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

void runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_standard_vector);
  RUN_TEST(test_little_endian_helpers);
  RUN_TEST(test_protocol_v2_playlist_metadata_integrity);
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
