#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>
#include <string.h>

namespace cyberclip {

namespace {
constexpr char kPreferencesNamespace[] = "wifi";
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr uint32_t kConnectFailureTimeoutMs = 15000;
}  // namespace

WifiManager wifiManager;

void WifiManager::begin() {
  loadFromPreferences();
  WiFi.mode(WIFI_OFF);
  WiFi.setHostname(status_.hostname);
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
  WiFi.disconnect(/*wifioff=*/true);
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
    WiFi.disconnect(/*wifioff=*/true);
    WiFi.mode(WIFI_OFF);
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
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(status_.hostname);
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

bool WifiManager::checkToken(const uint8_t *token, size_t length) const {
  if (!hasToken_ || !token || length != sizeof(token_)) return false;
  return memcmp(token, token_, sizeof(token_)) == 0;
}

}  // namespace cyberclip
