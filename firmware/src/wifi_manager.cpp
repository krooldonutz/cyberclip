#include "wifi_manager.h"

#include <DNSServer.h>
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
const IPAddress kHotspotIp(192, 168, 4, 1);
const IPAddress kHotspotGateway(192, 168, 4, 1);
const IPAddress kHotspotMask(255, 255, 255, 0);
DNSServer dnsServer;
}  // namespace

WifiManager wifiManager;

void WifiManager::begin() {
  loadFromPreferences();
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac()) & 0xFFFFFF;
  snprintf(hotspotSsid_, sizeof(hotspotSsid_), "CyberClip-%06X", suffix);
  snprintf(status_.hostname, sizeof(status_.hostname), "cyberclip-%06x",
           suffix);

  if (hotspotMode_ == HOTSPOT_ALWAYS ||
      (hotspotMode_ == HOTSPOT_FALLBACK &&
       (!enabled_ || !hasCredentials_))) {
    startHotspot();
  }
  if (enabled_ && hasCredentials_) connectIfNeeded();
}

void WifiManager::loadFromPreferences() {
  Preferences prefs;
  if (!prefs.begin(kPreferencesNamespace, /*readOnly=*/true)) return;
  enabled_ = prefs.getBool("enabled", false);
  const String storedSsid = prefs.getString("ssid", "");
  const String storedPass = prefs.getString("pass", "");
  const String hotspotPass = prefs.getString("ap_pass", "");
  hotspotMode_ = prefs.getUChar("ap_mode", HOTSPOT_OFF);
  lastEnabledHotspotMode_ =
      prefs.getUChar("ap_last", HOTSPOT_FALLBACK);
  if (!isValidHotspotMode(hotspotMode_)) hotspotMode_ = HOTSPOT_OFF;
  if (lastEnabledHotspotMode_ == HOTSPOT_OFF ||
      !isValidHotspotMode(lastEnabledHotspotMode_)) {
    lastEnabledHotspotMode_ = HOTSPOT_FALLBACK;
  }
  strlcpy(ssid_, storedSsid.c_str(), sizeof(ssid_));
  strlcpy(password_, storedPass.c_str(), sizeof(password_));
  strlcpy(hotspotPassword_, hotspotPass.c_str(), sizeof(hotspotPassword_));
  hasCredentials_ = storedSsid.length() > 0;
  hasToken_ = prefs.getBytes("token", token_, sizeof(token_)) == sizeof(token_);
  prefs.end();

  if (!isValidHotspotPasswordLength(strlen(hotspotPassword_))) {
    hotspotPassword_[0] = '\0';
    hotspotMode_ = HOTSPOT_OFF;
  }
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
  disconnectedAt_ = millis();
  connectIfNeeded();
  return true;
}

void WifiManager::clearCredentials() {
  ssid_[0] = '\0';
  password_[0] = '\0';
  hasCredentials_ = false;
  enabled_ = false;
  WiFi.disconnect(/*wifioff=*/false);
  WiFi.enableSTA(false);
  status_.state = WIFI_OFF;
  memset(status_.ip, 0, sizeof(status_.ip));
  disconnectedAt_ = 0;

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
    disconnectedAt_ = millis();
    connectIfNeeded();
  } else {
    WiFi.disconnect(/*wifioff=*/false);
    WiFi.enableSTA(false);
    status_.state = WIFI_OFF;
    memset(status_.ip, 0, sizeof(status_.ip));
    disconnectedAt_ = 0;
  }
}

bool WifiManager::setHotspotConfig(uint8_t mode, const char *password,
                                   size_t passwordLength) {
  if (!isValidHotspotMode(mode) || !password ||
      (passwordLength != 0 &&
       !isValidHotspotPasswordLength(passwordLength))) {
    return false;
  }
  if (passwordLength != 0) {
    memcpy(hotspotPassword_, password, passwordLength);
    hotspotPassword_[passwordLength] = '\0';
  }
  if (mode != HOTSPOT_OFF && hotspotPassword_[0] == '\0') return false;

  hotspotMode_ = mode;
  if (mode != HOTSPOT_OFF) lastEnabledHotspotMode_ = mode;
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putString("ap_pass", hotspotPassword_);
    prefs.end();
  }
  persistHotspotMode();
  if (mode == HOTSPOT_ALWAYS) startHotspot();
  if (mode == HOTSPOT_OFF) stopHotspotIfSafe();
  return true;
}

void WifiManager::toggleHotspotMode() {
  if (hotspotMode_ == HOTSPOT_OFF) {
    if (hotspotPassword_[0] == '\0') return;
    hotspotMode_ = lastEnabledHotspotMode_;
    if (hotspotMode_ == HOTSPOT_ALWAYS) startHotspot();
  } else {
    lastEnabledHotspotMode_ = hotspotMode_;
    hotspotMode_ = HOTSPOT_OFF;
    stopHotspotIfSafe();
  }
  persistHotspotMode();
}

void WifiManager::persistHotspotMode() {
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putUChar("ap_mode", hotspotMode_);
    prefs.putUChar("ap_last", lastEnabledHotspotMode_);
    prefs.end();
  }
}

void WifiManager::startHotspot() {
  if (hotspotRunning_ || hotspotPassword_[0] == '\0') return;
  WiFi.enableAP(true);
  WiFi.softAPConfig(kHotspotIp, kHotspotGateway, kHotspotMask);
  if (!WiFi.softAP(hotspotSsid_, hotspotPassword_)) return;
  hotspotRunning_ = true;
  dnsServer.start(53, "*", kHotspotIp);
  dnsRunning_ = true;
  wsServerBegin();
}

void WifiManager::stopHotspotIfSafe() {
  if (!hotspotRunning_ || wsHasActiveApClientOrTransfer()) return;
  if (dnsRunning_) {
    dnsServer.stop();
    dnsRunning_ = false;
  }
  WiFi.softAPdisconnect(/*wifioff=*/false);
  WiFi.enableAP(false);
  hotspotRunning_ = false;
}

void WifiManager::connectIfNeeded() {
  if (!enabled_ || !hasCredentials_ || WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (lastAttemptAt_ != 0 && now - lastAttemptAt_ < kReconnectIntervalMs) return;
  lastAttemptAt_ = now;
  if (disconnectedAt_ == 0) disconnectedAt_ = now;
  status_.state = WIFI_CONNECTING;
  // Add station capability without tearing down an active media AP.
  WiFi.enableSTA(true);
  WiFi.setHostname(status_.hostname);
  wsServerBegin();
  WiFi.begin(ssid_, password_);
}

void WifiManager::poll() {
  if (dnsRunning_) dnsServer.processNextRequest();
  const uint32_t now = millis();
  const bool stationUsable = enabled_ && hasCredentials_;
  const bool stationConnected =
      stationUsable && WiFi.status() == WL_CONNECTED;

  if (stationConnected) {
    status_.state = WIFI_CONNECTED;
    disconnectedAt_ = 0;
    const IPAddress ip = WiFi.localIP();
    for (size_t i = 0; i < 4; ++i) status_.ip[i] = ip[i];
  } else if (stationUsable) {
    memset(status_.ip, 0, sizeof(status_.ip));
    if (disconnectedAt_ == 0) disconnectedAt_ = now;
    const uint32_t disconnectedMs = now - disconnectedAt_;
    status_.state = disconnectedMs >= kConnectFailureTimeoutMs
                        ? WIFI_FAILED
                        : WIFI_CONNECTING;
    connectIfNeeded();
  }

  const uint32_t disconnectedMs =
      stationConnected ? 0 : (stationUsable ? now - disconnectedAt_ : 0);
  const bool shouldRun =
      hotspotPassword_[0] != '\0' &&
      shouldRunHotspot(hotspotMode_, stationUsable, stationConnected,
                       disconnectedMs, kConnectFailureTimeoutMs);
  if (shouldRun) startHotspot();
  else stopHotspotIfSafe();
}

WifiStatus WifiManager::status() const { return status_; }

HotspotStatus WifiManager::hotspotStatus() const {
  HotspotStatus result;
  result.mode = hotspotMode_;
  result.running = hotspotRunning_;
  result.passwordConfigured = hotspotPassword_[0] != '\0';
  strlcpy(result.ssid, hotspotSsid_, sizeof(result.ssid));
  if (hotspotRunning_) {
    const IPAddress ip = WiFi.softAPIP();
    for (size_t i = 0; i < 4; ++i) result.ip[i] = ip[i];
  }
  return result;
}

bool WifiManager::checkToken(const uint8_t *token, size_t length) const {
  if (!hasToken_ || !token || length != sizeof(token_)) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(token_); ++i) difference |= token[i] ^ token_[i];
  return difference == 0;
}

}  // namespace cyberclip
