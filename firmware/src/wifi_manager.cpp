#include "wifi_manager.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

#include "ws_server.h"

namespace cyberclip {

namespace {
constexpr char kPreferencesNamespace[] = "wifi";
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kConnectFailureTimeoutMs = 15000;
}  // namespace

WifiManager wifiManager;

void WifiManager::begin() {
  loadFromPreferences();

  // Unique per device (matches the MAC bytes used for the setup portal's
  // "Cyberclip-Setup-XXXX" AP name), so "<hostname>.local" identifies this
  // specific board even with several on the same network. Reading the MAC
  // works regardless of WiFi mode.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(status_.hostname, sizeof(status_.hostname), "cyberclip-%02x%02x",
           mac[4], mac[5]);

  // Deliberately leave WiFi/LWIP untouched here: WiFi.mode(WIFI_OFF) does
  // not bring up LWIP's tcpip task, and starting the WebSocket server (see
  // connectIfNeeded()) before that task exists crashes with a lwIP
  // "Invalid mbox" assertion. USB-only users who never provision WiFi
  // never touch the network stack at all this way.
  if (enabled_ && hasCredentials_) connectIfNeeded();
}

void WifiManager::loadFromPreferences() {
  Preferences prefs;
  if (!prefs.begin(kPreferencesNamespace, /*readOnly=*/true)) return;
  enabled_ = prefs.getBool("enabled", false);
  const String storedSsid = prefs.getString("ssid", "");
  const String storedPass = prefs.getString("pass", "");
  strlcpy(ssid_, storedSsid.c_str(), sizeof(ssid_));
  strlcpy(password_, storedPass.c_str(), sizeof(password_));
  hasCredentials_ = storedSsid.length() > 0;
  hasToken_ = prefs.getBytes("token", token_, sizeof(token_)) == sizeof(token_);
  prefs.end();
}

void WifiManager::generateToken() {
  esp_fill_random(token_, sizeof(token_));
  hasToken_ = true;
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putBytes("token", token_, sizeof(token_));
    prefs.end();
  }
}

bool WifiManager::setCredentials(const char *ssid, const char *password,
                                 uint8_t tokenOut[kWifiTokenSize],
                                 bool *tokenIncluded) {
  if (!ssid || !password) return false;
  const size_t ssidLength = strlen(ssid);
  const size_t passwordLength = strlen(password);
  if (ssidLength == 0 || ssidLength > kWifiMaxSsidLength ||
      passwordLength > kWifiMaxPasswordLength) {
    return false;
  }

  strlcpy(ssid_, ssid, sizeof(ssid_));
  strlcpy(password_, password, sizeof(password_));
  hasCredentials_ = true;
  enabled_ = true;

  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putBool("enabled", true);
    prefs.putString("ssid", ssid_);
    prefs.putString("pass", password_);
    prefs.end();
  }

  const bool firstPairing = !hasToken_;
  if (firstPairing) generateToken();
  if (tokenIncluded) *tokenIncluded = firstPairing;
  if (tokenOut) memcpy(tokenOut, token_, sizeof(token_));

  status_.state = WIFI_CONNECTING;
  lastAttemptAt_ = 0;
  connectIfNeeded();
  return true;
}

void WifiManager::clearCredentials() {
  ssid_[0] = '\0';
  password_[0] = '\0';
  hasCredentials_ = false;
  enabled_ = false;
  // wifioff=false: drop the AP association without tearing down LWIP's
  // netif/tcpip task, which the WebSocket server may already depend on
  // (see connectIfNeeded() / wsServerBegin()).
  WiFi.disconnect(/*wifioff=*/false);
  status_.state = WIFI_OFF;
  status_.ip[0] = status_.ip[1] = status_.ip[2] = status_.ip[3] = 0;

  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.remove("enabled");
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
  }
}

void WifiManager::setEnabled(bool enabled) {
  enabled_ = enabled;
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putBool("enabled", enabled);
    prefs.end();
  }
  if (enabled) {
    lastAttemptAt_ = 0;
    connectIfNeeded();
  } else {
    // wifioff=false: see clearCredentials() for why WiFi.mode(WIFI_OFF)
    // is avoided once the WebSocket server may have started.
    WiFi.disconnect(/*wifioff=*/false);
    status_.state = WIFI_OFF;
    status_.ip[0] = status_.ip[1] = status_.ip[2] = status_.ip[3] = 0;
  }
}

void WifiManager::connectIfNeeded() {
  if (!enabled_ || !hasCredentials_ || WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (lastAttemptAt_ != 0 && now - lastAttemptAt_ < kReconnectIntervalMs) return;
  lastAttemptAt_ = now;
  status_.state = WIFI_CONNECTING;
  // enableSTA (rather than WiFi.mode(WIFI_STA)) adds STA capability
  // without disturbing an access point the setup portal may already have
  // up - WiFi.mode() sets an absolute mode and would tear the AP down.
  WiFi.enableSTA(true);
  WiFi.setHostname(status_.hostname);
  // Only safe to start once WiFi has brought up LWIP's tcpip task (which
  // enableSTA(true) does); wsServerBegin() is idempotent, so repeated
  // reconnect attempts are harmless.
  wsServerBegin();
  WiFi.begin(ssid_, password_);
}

void WifiManager::poll() {
  if (!enabled_ || !hasCredentials_) return;

  if (WiFi.status() == WL_CONNECTED) {
    status_.state = WIFI_CONNECTED;
    const IPAddress ip = WiFi.localIP();
    status_.ip[0] = ip[0];
    status_.ip[1] = ip[1];
    status_.ip[2] = ip[2];
    status_.ip[3] = ip[3];
    // "<hostname>.local" then reaches the device without needing its IP -
    // MDNS.begin() registers it once; it keeps answering with the current
    // IP even across a later DHCP renewal, so this never needs restarting.
    if (!mdnsStarted_ && MDNS.begin(status_.hostname)) {
      mdnsStarted_ = true;
      MDNS.addService("ws", "tcp", 80);
    }
    return;
  }

  if (status_.state == WIFI_CONNECTED) {
    status_.ip[0] = status_.ip[1] = status_.ip[2] = status_.ip[3] = 0;
  }
  if (status_.state == WIFI_CONNECTING && lastAttemptAt_ != 0 &&
      millis() - lastAttemptAt_ > kConnectFailureTimeoutMs) {
    status_.state = WIFI_FAILED;
  }
  connectIfNeeded();
}

WifiStatus WifiManager::status() const { return status_; }

bool WifiManager::hasCredentials() const { return hasCredentials_; }

bool WifiManager::checkToken(const uint8_t *token, size_t length) const {
  if (!hasToken_ || !token || length != sizeof(token_)) return false;
  return memcmp(token, token_, sizeof(token_)) == 0;
}

}  // namespace cyberclip
