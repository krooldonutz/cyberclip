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
// Left behind by firmware that could join a home network.
constexpr const char *kRetiredPreferenceKeys[] = {"enabled", "ssid", "pass",
                                                  "token", "ap_last"};
const IPAddress kHotspotIp(192, 168, 4, 1);
const IPAddress kHotspotGateway(192, 168, 4, 1);
const IPAddress kHotspotMask(255, 255, 255, 0);
DNSServer dnsServer;
}  // namespace

WifiManager wifiManager;

void WifiManager::begin() {
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac()) & 0xFFFFFF;
  snprintf(hotspotSsid_, sizeof(hotspotSsid_), "CyberClip-%06X", suffix);
  loadFromPreferences();
  ensureHotspotPassword();
  if (hotspotMode_ == HOTSPOT_ON) startHotspot();
}

void WifiManager::loadFromPreferences() {
  Preferences prefs;
  if (!prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) return;
  const String hotspotPass = prefs.getString("ap_pass", "");
  hotspotMode_ = hotspotModeFromStored(prefs.getUChar("ap_mode", HOTSPOT_OFF));
  // Drop the home network name, password and pairing token older firmware
  // kept, so they don't linger in flash.
  for (const char *key : kRetiredPreferenceKeys) {
    if (prefs.isKey(key)) prefs.remove(key);
  }
  prefs.end();

  if (isValidHotspotPasswordLength(hotspotPass.length())) {
    strlcpy(hotspotPassword_, hotspotPass.c_str(), sizeof(hotspotPassword_));
  }
}

void WifiManager::ensureHotspotPassword() {
  if (hotspotPassword_[0] != '\0') return;
  uint8_t random[kHotspotGeneratedPasswordLength];
  esp_fill_random(random, sizeof(random));
  if (!formatHotspotPassword(hotspotPassword_, sizeof(hotspotPassword_),
                             random)) {
    return;
  }
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putString("ap_pass", hotspotPassword_);
    prefs.end();
  }
}

bool WifiManager::setHotspotMode(uint8_t mode) {
  if (!isValidHotspotMode(mode)) return false;
  hotspotMode_ = mode;
  persistHotspotMode();
  if (mode == HOTSPOT_ON) startHotspot();
  else stopHotspotIfSafe();
  return true;
}

void WifiManager::toggleHotspotMode() {
  setHotspotMode(hotspotMode_ == HOTSPOT_OFF ? HOTSPOT_ON : HOTSPOT_OFF);
}

void WifiManager::turnOffHotspot() { setHotspotMode(HOTSPOT_OFF); }

void WifiManager::suspend() {
  if (suspended_) return;
  suspended_ = true;
  if (dnsRunning_) {
    dnsServer.stop();
    dnsRunning_ = false;
  }
  hotspotRunning_ = false;
  WiFi.softAPdisconnect(/*wifioff=*/false);
  WiFi.mode(WIFI_OFF);
}

void WifiManager::resume() {
  if (!suspended_) return;
  suspended_ = false;
  // poll() restarts the hotspot if its setting asks for it.
}

void WifiManager::persistHotspotMode() {
  Preferences prefs;
  if (prefs.begin(kPreferencesNamespace, /*readOnly=*/false)) {
    prefs.putUChar("ap_mode", hotspotMode_);
    prefs.end();
  }
}

void WifiManager::startHotspot() {
  if (suspended_ || hotspotRunning_ || hotspotPassword_[0] == '\0') return;
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

void WifiManager::poll() {
  if (suspended_) return;
  if (dnsRunning_) dnsServer.processNextRequest();
  if (hotspotMode_ == HOTSPOT_ON) startHotspot();
  else stopHotspotIfSafe();
}

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

uint8_t WifiManager::hotspotStationCount() const {
  return hotspotRunning_ ? WiFi.softAPgetStationNum() : 0;
}

bool WifiManager::formatHotspotJoinQr(char *out, size_t outSize) const {
  if (hotspotPassword_[0] == '\0') return false;
  return formatWifiQrPayload(out, outSize, hotspotSsid_, hotspotPassword_) > 0;
}

}  // namespace cyberclip
