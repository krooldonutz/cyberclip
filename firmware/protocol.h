#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cyberclip {

constexpr uint8_t kMagic0 = 0x43;
constexpr uint8_t kMagic1 = 0x43;
constexpr uint8_t kProtocolVersion = 2;
constexpr uint32_t kMaxWirePayload = 4096;
constexpr uint16_t kMaxChunkData = 1024;
// HELLO_RESPONSE v2 bytes 0..14 retain the v1 layout. Byte 15 reports
// persistent storage support, and bytes 16..19 report max stored bytes.
constexpr size_t kHelloResponseFixedSize = 20;
constexpr uint8_t kPlaylistMetadataVersion = 2;
constexpr size_t kPlaylistMetadataHeaderSize = 10;
constexpr size_t kPlaylistMetadataCrcSize = 2;

// WiFi is an additional, optional, local-network-only transport alongside
// USB serial (see firmware/src/wifi_manager.h and firmware/src/ws_server.h).
// Credentials are provisioned once over USB; the pairing token this issues
// gates the WebSocket control transport.
constexpr size_t kWifiTokenSize = 16;
constexpr size_t kWifiMaxSsidLength = 32;
constexpr size_t kWifiMaxPasswordLength = 64;
constexpr size_t kHotspotMinPasswordLength = 8;
constexpr size_t kHotspotMaxPasswordLength = 63;

enum Command : uint8_t {
  HELLO = 0x01,
  // BEGIN_FRAME is 12 bytes for streaming. During playlist staging it appends
  // frameIndex u16 and delayMs u16 for a total of 16 bytes.
  BEGIN_FRAME = 0x10,
  FRAME_CHUNK = 0x11,
  COMMIT_FRAME = 0x12,
  CANCEL_TRANSFER = 0x13,
  SET_BACKLIGHT = 0x20,
  CLEAR_DISPLAY = 0x21,
  GET_STATUS = 0x22,
  // BEGIN_PLAYLIST: mediaType u8, frameCount u16, loop u8, rotation u8.
  BEGIN_PLAYLIST = 0x30,
  END_PLAYLIST = 0x31,
  PLAY_STORED = 0x32,
  CLEAR_STORED = 0x33,
  // SET_WIFI_CREDENTIALS: ssidLength u8, ssid bytes, passwordLength u8,
  // password bytes. Sent over USB during one-time pairing.
  SET_WIFI_CREDENTIALS = 0x40,
  GET_WIFI_STATUS = 0x41,
  CLEAR_WIFI_CREDENTIALS = 0x42,
  SET_WIFI_ENABLED = 0x43,
  // SET_HOTSPOT_CONFIG (USB only): mode u8, passwordLength u8, password bytes.
  // mode is HotspotMode. A zero passwordLength retains the stored password;
  // otherwise it must contain 8..63 WPA2 bytes. Enabling a mode without a
  // stored password returns INVALID_PAYLOAD.
  SET_HOTSPOT_CONFIG = 0x44,
  GET_HOTSPOT_STATUS = 0x45,
  HELLO_RESPONSE = 0x81,
  STATUS_RESPONSE = 0xA2,
  // WIFI_STATUS_RESPONSE: state u8, ipv4 4 bytes, hostnameLength u8,
  // hostname bytes, tokenIncluded u8, token 16 bytes (only when
  // tokenIncluded is 1, i.e. immediately after a first SET_WIFI_CREDENTIALS).
  WIFI_STATUS_RESPONSE = 0xA3,
  // HOTSPOT_STATUS_RESPONSE: configuredMode u8, running u8,
  // passwordConfigured u8, AP IPv4 4 bytes, ssidLength u8, SSID bytes.
  // The AP address is 192.168.4.1 while running and 0.0.0.0 otherwise.
  HOTSPOT_STATUS_RESPONSE = 0xA4,
  ACK = 0xF0,
  NACK = 0xF1,
};

enum WifiState : uint8_t {
  WIFI_OFF = 0,
  WIFI_CONNECTING = 1,
  WIFI_CONNECTED = 2,
  WIFI_FAILED = 3,
};

enum HotspotMode : uint8_t {
  HOTSPOT_OFF = 0,
  HOTSPOT_FALLBACK = 1,
  HOTSPOT_ALWAYS = 2,
};

inline bool isValidHotspotMode(uint8_t mode) {
  return mode <= HOTSPOT_ALWAYS;
}

inline bool isValidHotspotPasswordLength(size_t length) {
  return length >= kHotspotMinPasswordLength &&
         length <= kHotspotMaxPasswordLength;
}

inline bool shouldRunHotspot(uint8_t mode, bool stationUsable,
                             bool stationConnected,
                             uint32_t disconnectedMs,
                             uint32_t fallbackTimeoutMs) {
  if (mode == HOTSPOT_ALWAYS) return true;
  if (mode != HOTSPOT_FALLBACK) return false;
  return !stationUsable ||
         (!stationConnected && disconnectedMs >= fallbackTimeoutMs);
}

enum MediaType : uint8_t {
  MEDIA_IMAGE = 1,
  MEDIA_GIF = 2,
};

enum ErrorCode : uint8_t {
  UNSUPPORTED_VERSION = 1,
  UNSUPPORTED_COMMAND = 2,
  INVALID_PAYLOAD = 3,
  BAD_CRC = 4,
  BUSY = 5,
  OUT_OF_RANGE = 6,
  NO_MEMORY = 7,
  DECODE_FAILED = 8,
  NO_ACTIVE_TRANSFER = 9,
  SEQUENCE_ERROR = 10,
};

inline uint16_t crc16CcittFalseUpdate(uint16_t crc, uint8_t value) {
  crc ^= static_cast<uint16_t>(value) << 8;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                         : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

inline uint16_t crc16CcittFalse(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc = crc16CcittFalseUpdate(crc, data[i]);
  }
  return crc;
}

inline uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLe16(uint8_t *p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeLe32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

inline uint16_t uploadProgressPixels(uint16_t width, uint16_t frameCount,
                                     uint16_t completedFrames,
                                     uint32_t receivedBytes,
                                     uint32_t totalBytes) {
  if (width == 0 || frameCount == 0) return 0;
  if (completedFrames >= frameCount) return width;
  if (totalBytes == 0) {
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(width) * completedFrames) / frameCount);
  }
  if (receivedBytes > totalBytes) receivedBytes = totalBytes;
  const uint64_t completedUnits =
      static_cast<uint64_t>(completedFrames) * totalBytes + receivedBytes;
  const uint64_t totalUnits = static_cast<uint64_t>(frameCount) * totalBytes;
  return static_cast<uint16_t>(
      (static_cast<uint64_t>(width) * completedUnits) / totalUnits);
}

inline size_t playlistMetadataSize(uint8_t frameCount) {
  return kPlaylistMetadataHeaderSize +
         static_cast<size_t>(frameCount) * sizeof(uint16_t) +
         kPlaylistMetadataCrcSize;
}

// Stored metadata layout: "CCPL", version, generation, media type, frame
// count, rotation, loop, little-endian uint16 delays, then CRC-16.
inline bool validatePlaylistMetadata(const uint8_t *data, size_t length) {
  if (!data || length < playlistMetadataSize(1) ||
      data[0] != 'C' || data[1] != 'C' || data[2] != 'P' ||
      data[3] != 'L' || data[4] != kPlaylistMetadataVersion ||
      data[5] > 1 ||
      (data[6] != MEDIA_IMAGE && data[6] != MEDIA_GIF) ||
      data[7] == 0 || data[8] > 3 || data[9] > 1 ||
      (data[6] == MEDIA_IMAGE && data[7] != 1) ||
      length != playlistMetadataSize(data[7])) {
    return false;
  }
  const uint16_t expected = readLe16(data + length - kPlaylistMetadataCrcSize);
  return expected == crc16CcittFalse(data, length - kPlaylistMetadataCrcSize);
}

}  // namespace cyberclip
