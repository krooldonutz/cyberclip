#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../protocol.h"

namespace cyberclip {

struct HotspotStatus {
  uint8_t mode = HOTSPOT_OFF;
  bool running = false;
  bool passwordConfigured = false;
  uint8_t ip[4] = {0, 0, 0, 0};
  char ssid[kWifiMaxSsidLength + 1] = {};
};

// Owns the board's hotspot: its on/off setting and WPA2 password (NVS via
// Preferences) and its lifecycle. The board never joins another network.
// The password is generated on first boot (or kept from older firmware that
// let it be typed in) and reaches phones through the on-screen QR code.
class WifiManager {
 public:
  void begin();
  void poll();

  // Returns false if *mode* is not a HotspotMode.
  bool setHotspotMode(uint8_t mode);
  void toggleHotspotMode();
  // Turns the hotspot setting off. The hotspot itself stops once no phone
  // is mid-upload.
  void turnOffHotspot();

  // Turns the radio fully off without changing any stored settings;
  // resume() brings back whatever those settings ask for.
  void suspend();
  void resume();
  bool suspended() const { return suspended_; }
  // False means the radio is off, so the board may light-sleep.
  bool hotspotRunning() const { return hotspotRunning_; }

  HotspotStatus hotspotStatus() const;
  // Phones currently joined to the hotspot (0 while it is not running).
  uint8_t hotspotStationCount() const;
  // Writes the WIFI: QR text for joining the hotspot, which includes its
  // password. Returns false if it does not fit.
  bool formatHotspotJoinQr(char *out, size_t outSize) const;

 private:
  void loadFromPreferences();
  void ensureHotspotPassword();
  void startHotspot();
  void stopHotspotIfSafe();
  void persistHotspotMode();

  bool suspended_ = false;
  uint8_t hotspotMode_ = HOTSPOT_OFF;
  bool hotspotRunning_ = false;
  bool dnsRunning_ = false;
  char hotspotPassword_[kHotspotMaxPasswordLength + 1] = {};
  char hotspotSsid_[kWifiMaxSsidLength + 1] = {};
};

extern WifiManager wifiManager;

}  // namespace cyberclip
