#include <unity.h>

#include "../ams.h"
#include "../boot_guard.h"
#include "../protocol.h"
#include "../visualizer.h"

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

void test_stored_item_metadata_integrity() {
  uint8_t metadata[storedItemMetadataSize(2)] = {
      'C', 'C', 'M', 'I', kStoredItemMetadataVersion, 7, MEDIA_GIF, 2, 1, 1};
  writeLe32(metadata + 10, 54321);
  writeLe32(metadata + 14, 0xCAFEF00D);
  writeLe16(metadata + kStoredItemMetadataHeaderSize, 40);
  writeLe16(metadata + kStoredItemMetadataHeaderSize + 2, 90);
  writeLe16(metadata + sizeof(metadata) - kPlaylistMetadataCrcSize,
            crc16CcittFalse(metadata,
                            sizeof(metadata) - kPlaylistMetadataCrcSize));

  TEST_ASSERT_EQUAL_UINT32(24, sizeof(metadata));
  TEST_ASSERT_TRUE(validateStoredItemMetadata(metadata, sizeof(metadata), 7));
  // Metadata moved to another slot's file is rejected.
  TEST_ASSERT_FALSE(validateStoredItemMetadata(metadata, sizeof(metadata), 6));
  TEST_ASSERT_FALSE(
      validateStoredItemMetadata(metadata, sizeof(metadata) - 1, 7));
  metadata[12] ^= 1;
  TEST_ASSERT_FALSE(validateStoredItemMetadata(metadata, sizeof(metadata), 7));
}

void test_stored_item_paths_and_slots() {
  char path[kStoredPathSize];
  storedFramePath(path, sizeof(path), 15, 254);
  TEST_ASSERT_EQUAL_STRING("/s15f254.jpg", path);
  storedFramePath(path, sizeof(path), 0, 0);
  TEST_ASSERT_EQUAL_STRING("/s00f000.jpg", path);
  storedMetadataPath(path, sizeof(path), 3, false);
  TEST_ASSERT_EQUAL_STRING("/s03.meta", path);
  storedMetadataPath(path, sizeof(path), 3, true);
  TEST_ASSERT_EQUAL_STRING("/s03.tmp", path);

  TEST_ASSERT_EQUAL_UINT8(15, storedFileSlot("/s15f254.jpg"));
  TEST_ASSERT_EQUAL_UINT8(3, storedFileSlot("s03.meta"));
  TEST_ASSERT_EQUAL_UINT8(3, storedFileSlot("s03.tmp"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot("s16.meta"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot("s03.metax"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot("s3f001.jpg"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot("active"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot("a001.jpg"));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, storedFileSlot(nullptr));

  TEST_ASSERT_TRUE(isLegacyFrameName("/a000.jpg"));
  TEST_ASSERT_TRUE(isLegacyFrameName("b254.jpg"));
  TEST_ASSERT_FALSE(isLegacyFrameName("c000.jpg"));
  TEST_ASSERT_FALSE(isLegacyFrameName("playlist.meta"));
  TEST_ASSERT_FALSE(isLegacyFrameName("s00f000.jpg"));
}

void test_stored_slot_selection() {
  bool used[kMaxStoredItems] = {};
  TEST_ASSERT_EQUAL_UINT8(0, firstFreeStoredSlot(used));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, firstUsedStoredSlot(used));
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, nextStoredSlot(used, 4));

  used[0] = used[1] = used[5] = true;
  TEST_ASSERT_EQUAL_UINT8(2, firstFreeStoredSlot(used));
  TEST_ASSERT_EQUAL_UINT8(0, firstUsedStoredSlot(used));
  TEST_ASSERT_EQUAL_UINT8(5, nextStoredSlot(used, 1));
  // Wraps around, and never returns the slot it started from.
  TEST_ASSERT_EQUAL_UINT8(0, nextStoredSlot(used, 5));
  used[0] = used[1] = false;
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, nextStoredSlot(used, 5));

  for (bool &slot : used) slot = true;
  TEST_ASSERT_EQUAL_UINT8(kNoStoredSlot, firstFreeStoredSlot(used));
  TEST_ASSERT_EQUAL_UINT8(0, nextStoredSlot(used, kMaxStoredItems - 1));
}

void test_stored_item_position() {
  bool used[kMaxStoredItems] = {};
  uint8_t position = 9;
  uint8_t count = 9;
  TEST_ASSERT_FALSE(storedItemPosition(used, 0, &position, &count));
  TEST_ASSERT_EQUAL_UINT8(0, count);

  used[1] = used[4] = used[9] = true;
  TEST_ASSERT_TRUE(storedItemPosition(used, 4, &position, &count));
  TEST_ASSERT_EQUAL_UINT8(2, position);
  TEST_ASSERT_EQUAL_UINT8(3, count);
  TEST_ASSERT_TRUE(storedItemPosition(used, 9, &position, &count));
  TEST_ASSERT_EQUAL_UINT8(3, position);
  TEST_ASSERT_FALSE(storedItemPosition(used, 2, &position, &count));
}

void test_battery_button_hold() {
  TEST_ASSERT_FALSE(isBatteryButtonHeld(1000, 1000));
  TEST_ASSERT_FALSE(isBatteryButtonHeld(1000, 1499));
  TEST_ASSERT_TRUE(isBatteryButtonHeld(1000, 1500));
  // millis() wrapping around does not turn a tap into a hold.
  TEST_ASSERT_FALSE(isBatteryButtonHeld(UINT32_MAX - 100, 200));
  TEST_ASSERT_TRUE(isBatteryButtonHeld(UINT32_MAX - 100, 450));
}

void test_stored_media_command_bytes() {
  TEST_ASSERT_EQUAL_HEX8(0x34, LIST_STORED);
  TEST_ASSERT_EQUAL_HEX8(0x35, SELECT_STORED);
  TEST_ASSERT_EQUAL_HEX8(0x36, DELETE_STORED);
  TEST_ASSERT_EQUAL_HEX8(0x37, READ_STORED_FRAME);
  TEST_ASSERT_EQUAL_HEX8(0xA5, STORED_LIST_RESPONSE);
  TEST_ASSERT_EQUAL_HEX8(0xA6, STORED_FRAME_RESPONSE);
  TEST_ASSERT_EQUAL_UINT8(11, STORAGE_FULL);
  TEST_ASSERT_EQUAL_UINT16(4085, kMaxStoredFrameReadData);
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

  // Late GIF frames: a streak builds only from frames at least 8 ms late.
  TEST_ASSERT_EQUAL_UINT8(0, updateLateFrameStreak(0, 0));
  TEST_ASSERT_EQUAL_UINT8(0, updateLateFrameStreak(0, 7));
  TEST_ASSERT_EQUAL_UINT8(1, updateLateFrameStreak(0, 8));
  TEST_ASSERT_EQUAL_UINT8(3, updateLateFrameStreak(2, 500));
  TEST_ASSERT_EQUAL_UINT8(0, updateLateFrameStreak(2, 3));
  TEST_ASSERT_EQUAL_UINT8(255, updateLateFrameStreak(255, 100));

  // Backlight is halved on battery but never turned off by it.
  TEST_ASSERT_EQUAL_UINT8(128, backlightLevelFor(false, 128));
  TEST_ASSERT_EQUAL_UINT8(255, backlightLevelFor(false, 255));
  TEST_ASSERT_EQUAL_UINT8(64, backlightLevelFor(true, 128));
  TEST_ASSERT_EQUAL_UINT8(127, backlightLevelFor(true, 255));
  TEST_ASSERT_EQUAL_UINT8(1, backlightLevelFor(true, 1));
  TEST_ASSERT_EQUAL_UINT8(0, backlightLevelFor(true, 0));

  TEST_ASSERT_FALSE(shouldSuspendWifi(false, false, 600000, 120000));
  TEST_ASSERT_FALSE(shouldSuspendWifi(true, false, 119999, 120000));
  TEST_ASSERT_TRUE(shouldSuspendWifi(true, false, 120000, 120000));
  TEST_ASSERT_FALSE(shouldSuspendWifi(true, true, 600000, 120000));

  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(true, false, 0));
  TEST_ASSERT_EQUAL_UINT32(1, loopIdleSleepMs(false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(false, true, 1));
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(false, true, -5));
  TEST_ASSERT_EQUAL_UINT32(1, loopIdleSleepMs(false, true, 2));

  // Longer battery idle sleep, still capped by the next GIF frame.
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(true, false, 0, 20));
  TEST_ASSERT_EQUAL_UINT32(20, loopIdleSleepMs(false, false, 0, 20));
  TEST_ASSERT_EQUAL_UINT32(9, loopIdleSleepMs(false, true, 10, 20));
  TEST_ASSERT_EQUAL_UINT32(20, loopIdleSleepMs(false, true, 500, 20));
  TEST_ASSERT_EQUAL_UINT32(0, loopIdleSleepMs(false, true, 1, 20));
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

void test_ams_playback_info_and_time() {
  ams::PlaybackInfo info;
  TEST_ASSERT_TRUE(ams::parsePlaybackInfo("1,1.000,42.500", 14, &info));
  TEST_ASSERT_EQUAL_UINT8(ams::PLAYBACK_PLAYING, info.state);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, info.rate);
  TEST_ASSERT_EQUAL_FLOAT(42.5f, info.elapsedSeconds);
  TEST_ASSERT_TRUE(ams::parsePlaybackInfo("2,-2.0,10", 9, &info));
  TEST_ASSERT_EQUAL_FLOAT(-2.0f, info.rate);
  TEST_ASSERT_FALSE(ams::parsePlaybackInfo("1,1.0", 5, &info));
  TEST_ASSERT_FALSE(ams::parsePlaybackInfo("1,x,3", 5, &info));
  TEST_ASSERT_FALSE(ams::parsePlaybackInfo("1", 1, &info));

  float seconds = 0;
  TEST_ASSERT_TRUE(ams::parseSeconds("245.123", 7, &seconds));
  TEST_ASSERT_EQUAL_FLOAT(245.123f, seconds);
  TEST_ASSERT_FALSE(ams::parseSeconds("", 0, &seconds));
  TEST_ASSERT_FALSE(ams::parseSeconds("abc", 3, &seconds));
  TEST_ASSERT_FALSE(ams::parseSeconds("-1", 2, &seconds));

  ams::PlaybackInfo playing;
  playing.rate = 1.0f;
  playing.elapsedSeconds = 10.0f;
  TEST_ASSERT_EQUAL_FLOAT(12.5f, ams::extrapolateElapsed(playing, 2500, 200));
  TEST_ASSERT_EQUAL_FLOAT(200.0f, ams::extrapolateElapsed(playing, 999000, 200));
  playing.rate = -2.0f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, ams::extrapolateElapsed(playing, 9000, 200));

  char text[12];
  ams::formatTrackTime(65.9f, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("1:05", text);
  ams::formatTrackTime(3725.0f, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("1:02:05", text);
  ams::formatTrackTime(-3.0f, text, sizeof(text));
  TEST_ASSERT_EQUAL_STRING("0:00", text);
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

  TEST_ASSERT_EQUAL_UINT32(1, ams::utf8Next(kana, 4, 0));
  TEST_ASSERT_EQUAL_UINT32(4, ams::utf8Next(kana, 4, 1));
  TEST_ASSERT_EQUAL_UINT32(4, ams::utf8Next(kana, 4, 4));

  char buffer[5] = "";
  TEST_ASSERT_TRUE(ams::storeUtf8(buffer, sizeof(buffer), cafe, 5));
  TEST_ASSERT_EQUAL_STRING("caf", buffer);
  TEST_ASSERT_FALSE(ams::storeUtf8(buffer, sizeof(buffer), cafe, 5));
  TEST_ASSERT_TRUE(ams::storeUtf8(buffer, sizeof(buffer), "", 0));
  TEST_ASSERT_EQUAL_STRING("", buffer);
  TEST_ASSERT_FALSE(ams::storeUtf8(buffer, sizeof(buffer), "", 0));
}

void test_visualizer_wave_and_color() {
  using namespace visualizer;
  WaveParams params;
  params.speed1 = 3.0f;
  params.speed2 = 5.0f;
  params.cycles1 = 1.0f;
  params.cycles2 = 2.0f;
  params.phase1 = 0.0f;
  params.phase2 = 0.0f;
  // sin(0) = 0 and cos(0) = 1 at t = 0, position 0.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.70f, waveLevel(params, 0, 0));
  for (int i = 0; i <= 100; ++i) {
    const float level = waveLevel(params, i * 0.137f, i / 100.0f);
    TEST_ASSERT_TRUE(level >= 0.1f - 0.0001f && level <= 1.0f + 0.0001f);
  }

  const uint32_t low[6] = {0, 0, 0, 0, 0, 0};
  const WaveParams lowest = randomWaveParams(low);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, lowest.speed1);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, lowest.cycles1);
  const uint32_t high[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                            0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  const WaveParams highest = randomWaveParams(high);
  TEST_ASSERT_TRUE(highest.speed1 <= 5.0f && highest.speed2 <= 7.0f);

  TEST_ASSERT_EQUAL_HEX16(0xF800, hsvToRgb565(0, 1, 1));
  TEST_ASSERT_EQUAL_HEX16(0x07E0, hsvToRgb565(120, 1, 1));
  TEST_ASSERT_EQUAL_HEX16(0x001F, hsvToRgb565(240, 1, 1));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, hsvToRgb565(77, 0, 1));
  TEST_ASSERT_EQUAL_HEX16(0xF800, hsvToRgb565(360, 1, 1));
  // Saturation is capped, so a song color is never pure white or black.
  const uint16_t color = randomSongColor(0x80000000, 0);
  TEST_ASSERT_NOT_EQUAL(0xFFFF, color);
  TEST_ASSERT_NOT_EQUAL(0x0000, color);
}

void test_boot_guard_escalates_crash_loops() {
  BootGuardState state = {0xDEADBEEF, 1234};  // garbage after power-on
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kNormal);
  TEST_ASSERT_EQUAL_UINT32(1, state.crashes);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kNormal);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kSafeMode);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kSafeMode);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kDownloadMode);
  // The count starts over for the freshly installed firmware.
  TEST_ASSERT_EQUAL_UINT32(0, state.crashes);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kNormal);
}

void test_boot_guard_clears_on_clean_boot_or_stable_run() {
  BootGuardState state = {kBootGuardMagic, 0};
  bootGuardOnBoot(state, true);
  bootGuardOnBoot(state, true);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, false) == BootAction::kNormal);
  TEST_ASSERT_EQUAL_UINT32(0, state.crashes);

  bootGuardOnBoot(state, true);
  bootGuardOnBoot(state, true);
  bootGuardMarkStable(state);
  TEST_ASSERT_TRUE(bootGuardOnBoot(state, true) == BootAction::kNormal);
}

void runTests() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_guard_escalates_crash_loops);
  RUN_TEST(test_boot_guard_clears_on_clean_boot_or_stable_run);
  RUN_TEST(test_battery_percent_and_level);
  RUN_TEST(test_crc_standard_vector);
  RUN_TEST(test_little_endian_helpers);
  RUN_TEST(test_upload_progress_pixels);
  RUN_TEST(test_protocol_v2_playlist_metadata_integrity);
  RUN_TEST(test_stored_item_metadata_integrity);
  RUN_TEST(test_stored_item_paths_and_slots);
  RUN_TEST(test_stored_slot_selection);
  RUN_TEST(test_stored_media_command_bytes);
  RUN_TEST(test_stored_item_position);
  RUN_TEST(test_battery_button_hold);
  RUN_TEST(test_wifi_command_bytes_do_not_collide);
  RUN_TEST(test_hotspot_validation_and_fallback_policy);
  RUN_TEST(test_power_policy);
  RUN_TEST(test_ams_entity_update_parsing);
  RUN_TEST(test_ams_playback_state);
  RUN_TEST(test_ams_playback_info_and_time);
  RUN_TEST(test_ams_utf8_storage);
  RUN_TEST(test_visualizer_wave_and_color);
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
