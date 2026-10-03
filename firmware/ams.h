#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Apple Media Service (AMS): the GATT service an iPhone exposes to a paired
// BLE accessory so it can read what is playing. The accessory writes the
// attributes it wants to the Entity Update characteristic, and iOS then
// notifies each value whenever it changes. See Apple's "Apple Media Service
// Specification".
namespace cyberclip {
namespace ams {

constexpr char kServiceUuid[] = "89D3502B-0F36-433A-8EF4-C502AD55F8DC";
constexpr char kRemoteCommandUuid[] = "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2";
constexpr char kEntityUpdateUuid[] = "2F7CABCE-808D-411F-9A0C-BB92BA96C102";
constexpr char kEntityAttributeUuid[] = "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7";

// The service UUID in the little-endian byte order used in advertising data.
constexpr uint8_t kServiceUuidLe[16] = {0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5,
                                        0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F,
                                        0x2B, 0x50, 0xD3, 0x89};

enum EntityId : uint8_t { ENTITY_PLAYER = 0, ENTITY_QUEUE = 1, ENTITY_TRACK = 2 };

enum PlayerAttribute : uint8_t {
  PLAYER_NAME = 0,
  PLAYER_PLAYBACK_INFO = 1,
  PLAYER_VOLUME = 2,
};

enum TrackAttribute : uint8_t {
  TRACK_ARTIST = 0,
  TRACK_ALBUM = 1,
  TRACK_TITLE = 2,
  TRACK_DURATION = 3,
};

enum PlaybackState : uint8_t {
  PLAYBACK_PAUSED = 0,
  PLAYBACK_PLAYING = 1,
  PLAYBACK_REWINDING = 2,
  PLAYBACK_FAST_FORWARDING = 3,
  PLAYBACK_UNKNOWN = 0xff,
};

constexpr uint8_t kEntityUpdateFlagTruncated = 0x01;

// Entity Update writes that register for the attributes Cyberclip shows.
constexpr uint8_t kTrackSubscription[] = {ENTITY_TRACK, TRACK_ARTIST,
                                          TRACK_TITLE};
constexpr uint8_t kPlayerSubscription[] = {ENTITY_PLAYER,
                                           PLAYER_PLAYBACK_INFO};

struct EntityUpdate {
  uint8_t entity = 0;
  uint8_t attribute = 0;
  bool truncated = false;
  const char *value = nullptr;  // UTF-8, not NUL-terminated
  size_t valueLength = 0;
};

// Splits an Entity Update notification: EntityID, AttributeID, flags, value.
inline bool parseEntityUpdate(const uint8_t *data, size_t length,
                              EntityUpdate *update) {
  if (!data || !update || length < 3) return false;
  update->entity = data[0];
  update->attribute = data[1];
  update->truncated = (data[2] & kEntityUpdateFlagTruncated) != 0;
  update->value = reinterpret_cast<const char *>(data + 3);
  update->valueLength = length - 3;
  return true;
}

// PlaybackInfo is "<state>,<rate>,<elapsed seconds>"; only the state is used.
inline PlaybackState parsePlaybackState(const char *value, size_t length) {
  if (!value || length == 0 || value[0] < '0' || value[0] > '3') {
    return PLAYBACK_UNKNOWN;
  }
  if (length > 1 && value[1] != ',') return PLAYBACK_UNKNOWN;
  return static_cast<PlaybackState>(value[0] - '0');
}

// Largest length <= maxLength that does not split a UTF-8 sequence.
inline size_t utf8Boundary(const char *text, size_t length, size_t maxLength) {
  if (length <= maxLength) return length;
  size_t end = maxLength;
  // Step back over continuation bytes (10xxxxxx) to the start of the
  // sequence that crosses the limit, and drop that whole sequence.
  while (end > 0 &&
         (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
    --end;
  }
  return end;
}

// Start of the UTF-8 character after the one at *position*.
inline size_t utf8Next(const char *text, size_t length, size_t position) {
  if (position >= length) return length;
  ++position;
  while (position < length &&
         (static_cast<uint8_t>(text[position]) & 0xC0) == 0x80) {
    ++position;
  }
  return position;
}

// Copies a value into a NUL-terminated buffer, cutting only at a UTF-8
// character boundary. Returns true when the stored text changed.
inline bool storeUtf8(char *destination, size_t capacity, const char *value,
                      size_t length) {
  if (!destination || capacity == 0) return false;
  const size_t copied = utf8Boundary(value, length, capacity - 1);
  if (strlen(destination) == copied &&
      (copied == 0 || memcmp(destination, value, copied) == 0)) {
    return false;
  }
  if (copied > 0) memcpy(destination, value, copied);
  destination[copied] = '\0';
  return true;
}

}  // namespace ams
}  // namespace cyberclip
