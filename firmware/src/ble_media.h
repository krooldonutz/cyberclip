#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../ams.h"

namespace cyberclip {

constexpr size_t kNowPlayingTextSize = 128;

struct NowPlaying {
  // An iPhone is paired, connected, and sending AMS updates.
  bool connected = false;
  uint8_t playback = ams::PLAYBACK_UNKNOWN;
  char title[kNowPlayingTextSize] = {};
  char artist[kNowPlayingTextSize] = {};
  // Incremented whenever any field above changes.
  uint32_t revision = 0;
};

// Reads what an iPhone is playing over BLE through the Apple Media Service.
// Cyberclip advertises as a BLE peripheral; once the iPhone pairs with it
// (Settings > Bluetooth), it subscribes to the track title and artist and
// the playback state. Updates arrive on the NimBLE host task, so the main
// loop reads them through snapshot() instead of touching shared state.
class BleMedia {
 public:
  void begin();

  uint32_t revision() const;
  NowPlaying snapshot() const;

  // Called from BLE callbacks and the setup task only.
  void onConnected(uint16_t connHandle);
  void onEncrypted(uint16_t connHandle);
  void onDisconnected();
  void onEntityUpdate(const uint8_t *data, size_t length);
  void subscribePendingConnection();

 private:
  void markChanged();

  NowPlaying state_;
  uint16_t pendingConnHandle_ = 0xffff;
  void *setupTask_ = nullptr;
};

extern BleMedia bleMedia;

}  // namespace cyberclip
