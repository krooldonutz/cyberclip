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

// Owns WiFi credentials/pairing-token storage (NVS via Preferences) and the
// station connection lifecycle. WiFi stays off until credentials are
// provisioned over USB, so a USB-only user never pays for an idle radio.
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

  WifiStatus status() const;
  bool checkToken(const uint8_t *token, size_t length) const;

 private:
  void loadFromPreferences();
  void generateToken();
  void connectIfNeeded();

  bool enabled_ = false;
  bool hasCredentials_ = false;
  bool hasToken_ = false;
  uint8_t token_[kWifiTokenSize] = {};
  char ssid_[kWifiMaxSsidLength + 1] = {};
  char password_[kWifiMaxPasswordLength + 1] = {};
  WifiStatus status_;
  uint32_t lastAttemptAt_ = 0;
};

extern WifiManager wifiManager;

}  // namespace cyberclip
