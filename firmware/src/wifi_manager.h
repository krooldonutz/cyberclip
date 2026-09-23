#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../protocol.h"

namespace cyberclip {

struct WifiStatus {
  uint8_t state = WIFI_OFF;
  uint8_t ip[4] = {0, 0, 0, 0};
  char hostname[33] = "cyberclip";
};

struct HotspotStatus {
  uint8_t mode = HOTSPOT_OFF;
  bool running = false;
  bool passwordConfigured = false;
  uint8_t ip[4] = {0, 0, 0, 0};
  char ssid[kWifiMaxSsidLength + 1] = {};
};

// Owns station and hotspot settings (NVS via Preferences) and their lifecycle.
class WifiManager {
 public:
  void begin();
  void poll();

  // Stores new credentials, enables WiFi, and starts connecting. Generates
  // a pairing token on the first-ever call and reports it via *tokenOut*/
  // *tokenIncluded* so the caller can echo it back over USB once. Returns
  // false if the ssid/password are invalid.
  bool setCredentials(const char *ssid, const char *password,
                      uint8_t tokenOut[kWifiTokenSize], bool *tokenIncluded);
  void clearCredentials();
  void setEnabled(bool enabled);
  bool setHotspotConfig(uint8_t mode, const char *password,
                        size_t passwordLength);
  void toggleHotspotMode();

  WifiStatus status() const;
  HotspotStatus hotspotStatus() const;
  bool checkToken(const uint8_t *token, size_t length) const;

 private:
  void loadFromPreferences();
  void generateToken();
  void connectIfNeeded();
  void startHotspot();
  void stopHotspotIfSafe();
  void persistHotspotMode();

  bool enabled_ = false;
  bool hasCredentials_ = false;
  bool hasToken_ = false;
  uint8_t token_[kWifiTokenSize] = {};
  char ssid_[kWifiMaxSsidLength + 1] = {};
  char password_[kWifiMaxPasswordLength + 1] = {};
  WifiStatus status_;
  uint32_t lastAttemptAt_ = 0;
  uint32_t disconnectedAt_ = 0;
  uint8_t hotspotMode_ = HOTSPOT_OFF;
  uint8_t lastEnabledHotspotMode_ = HOTSPOT_FALLBACK;
  bool hotspotRunning_ = false;
  bool dnsRunning_ = false;
  char hotspotPassword_[kHotspotMaxPasswordLength + 1] = {};
  char hotspotSsid_[kWifiMaxSsidLength + 1] = {};
};

extern WifiManager wifiManager;

}  // namespace cyberclip
