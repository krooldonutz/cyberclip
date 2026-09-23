#include "setup_portal.h"

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "../protocol.h"
#include "display_hooks.h"
#include "wifi_manager.h"
#include "ws_server.h"

namespace cyberclip {

namespace {

constexpr size_t kMaxScannedNetworks = 20;
// How long the setup access point stays up after WiFi connects, so the
// phone's still-open setup page has time to poll /setup/status and let the
// user tap the "Open Cyberclip" link before it disappears - see
// setupPortalPoll().
constexpr uint32_t kApGracePeriodMs = 20000;

DNSServer dnsServer;

bool active = false;
bool routesRegistered = false;
uint8_t pendingToken[kWifiTokenSize] = {};
bool havePendingToken = false;

// Populated once, the moment WIFI_CONNECTED is first observed (see
// setupPortalPoll()); read by handleSetupStatus() from the AsyncWebServer's
// own task with no explicit lock - a request landing mid-write could in
// theory see a partially-written string, self-correcting on the next poll
// a second later. Not worth a mutex for a cosmetic status field.
uint32_t connectedAt = 0;
char cachedHost[48] = {};
char cachedHandoffUrl[192] = {};

struct ScannedNetwork {
  String ssid;
  int32_t rssi;
  bool secure;
};
std::vector<ScannedNetwork> scannedNetworks;

String buildSetupSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "Cyberclip-Setup-%02X%02X", mac[4], mac[5]);
  return String(ssid);
}

// Runs once before the setup access point comes up (see setupPortalBegin())
// - scanning and hosting an AP at the same time is best avoided, so this
// briefly delays the AP appearing by a couple of seconds in exchange for
// letting the setup page show a tappable list of nearby networks instead of
// requiring the SSID to be typed exactly.
void scanNearbyNetworks() {
  scannedNetworks.clear();
  WiFi.scanDelete();
  const int16_t count = WiFi.scanNetworks();
  for (int16_t i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    const int32_t rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    bool merged = false;
    for (auto &network : scannedNetworks) {
      if (network.ssid != ssid) continue;
      if (rssi > network.rssi) {
        network.rssi = rssi;
        network.secure = secure;
      }
      merged = true;
      break;
    }
    if (!merged) scannedNetworks.push_back({ssid, rssi, secure});
  }
  WiFi.scanDelete();

  std::sort(scannedNetworks.begin(), scannedNetworks.end(),
            [](const ScannedNetwork &a, const ScannedNetwork &b) {
              return a.rssi > b.rssi;
            });
  if (scannedNetworks.size() > kMaxScannedNetworks) {
    scannedNetworks.resize(kMaxScannedNetworks);
  }
}

String jsonEscaped(const String &value) {
  String escaped;
  escaped.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (static_cast<unsigned char>(c) < 0x20) continue;
    if (c == '"' || c == '\\') escaped += '\\';
    escaped += c;
  }
  return escaped;
}

const char kSetupPage[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cyberclip setup</title>
<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}
input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;font-size:16px}
button{width:100%;padding:12px;font-size:16px;margin-top:12px}
.network{padding:10px 12px;border:1px solid #ccc;border-radius:6px;margin:6px 0;
  cursor:pointer;display:flex;justify-content:space-between;align-items:center}
.network.selected{border-color:#2563eb;border-width:2px;background:#eff6ff}
.muted{color:#666;font-size:14px}
a{color:#2563eb}</style></head>
<body><h1>Connect Cyberclip to WiFi</h1>
<div id="networks" class="muted">Scanning for networks&hellip;</div>
<p class="muted"><a href="#" id="manual-toggle">Enter a network name manually</a></p>
<form method="POST" action="/setup" id="form">
<label id="manual-label" style="display:none">Network name (SSID)
<input id="manual-ssid" maxlength="32">
</label>
<input type="hidden" id="ssid" name="ssid">
<label>Password (leave blank for an open network)
<input name="password" type="password" maxlength="64">
</label>
<button type="submit">Connect</button>
</form>
<script>
const networksEl = document.getElementById('networks');
const ssidField = document.getElementById('ssid');
const manualSsid = document.getElementById('manual-ssid');
const manualLabel = document.getElementById('manual-label');
const manualToggle = document.getElementById('manual-toggle');

manualToggle.addEventListener('click', (event) => {
  event.preventDefault();
  manualLabel.style.display = 'block';
  networksEl.style.display = 'none';
  manualToggle.style.display = 'none';
  manualSsid.focus();
});
manualSsid.addEventListener('input', () => { ssidField.value = manualSsid.value; });

async function loadNetworks() {
  try {
    const response = await fetch('/networks');
    const networks = await response.json();
    if (!networks.length) {
      networksEl.textContent = 'No networks found nearby - enter one manually below.';
      return;
    }
    networksEl.textContent = '';
    for (const network of networks) {
      const row = document.createElement('div');
      row.className = 'network';
      row.textContent = network.ssid + (network.secure ? ' \uD83D\uDD12' : '');
      row.addEventListener('click', () => {
        for (const child of networksEl.children) child.classList.remove('selected');
        row.classList.add('selected');
        ssidField.value = network.ssid;
      });
      networksEl.appendChild(row);
    }
  } catch (error) {
    networksEl.textContent = 'Could not scan for networks - enter one manually below.';
  }
}
document.getElementById('form').addEventListener('submit', (event) => {
  if (!ssidField.value) {
    event.preventDefault();
    alert('Choose a network from the list, or enter one manually, first.');
  }
});
loadNetworks();
</script></body></html>
)HTML";

const char kConnectingPage[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title>
<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px;text-align:center}
.button{display:block;padding:12px;font-size:16px;margin-top:16px;border-radius:8px;
  background:#2563eb;color:#fff;text-decoration:none}
.muted{color:#666;font-size:14px}</style>
</head><body><h1 id="msg">Connecting&hellip;</h1>
<p id="detail">Keep this page open.</p>
<script>
async function poll() {
  try {
    const response = await fetch('/setup/status');
    const status = await response.json();
    if (status.state === 2) {
      document.getElementById('msg').textContent = 'Connected!';
      if (status.url) {
        document.getElementById('detail').innerHTML =
          'Tap below once your phone has switched back to your normal WiFi or data ' +
          '(usually just a few seconds):<br>' +
          '<a class="button" href="' + status.url + '">Open Cyberclip</a>' +
          '<p class="muted">Still on the board\'s screen too, in case that\'s easier.</p>';
      } else {
        document.getElementById('detail').textContent =
          "Open the Cyberclip app and connect over WiFi to " + status.host +
          " (also shown on the board's screen).";
      }
      return;
    }
    if (status.state === 3) {
      document.getElementById('msg').textContent = 'Could not connect';
      document.getElementById('detail').innerHTML =
        'Check the network name and password and <a href="/">try again</a>.';
      return;
    }
  } catch (error) {
    // The access point may already be shutting down after success.
  }
  setTimeout(poll, 1000);
}
poll();
</script></body></html>
)HTML";

void handleSetupSubmit(AsyncWebServerRequest *request) {
  if (!request->hasArg("ssid")) {
    request->redirect("/");
    return;
  }
  const String ssid = request->arg("ssid");
  const String password = request->arg("password");

  bool tokenIncluded = false;
  const bool accepted = wifiManager.setCredentials(
      ssid.c_str(), password.c_str(), pendingToken, &tokenIncluded);
  if (!accepted) {
    request->redirect("/");
    return;
  }
  havePendingToken = havePendingToken || tokenIncluded;
  request->send_P(200, "text/html", kConnectingPage);
}

void handleSetupStatus(AsyncWebServerRequest *request) {
  const uint8_t state = wifiManager.status().state;
  String body = "{\"state\":";
  body += String(state);
  if (state == WIFI_CONNECTED) {
    body += ",\"host\":\"";
    body += jsonEscaped(cachedHost);
    body += '"';
    if (cachedHandoffUrl[0] != '\0') {
      body += ",\"url\":\"";
      body += jsonEscaped(cachedHandoffUrl);
      body += '"';
    }
  }
  body += '}';
  request->send(200, "application/json", body);
}

void handleNetworksList(AsyncWebServerRequest *request) {
  String body = "[";
  for (size_t i = 0; i < scannedNetworks.size(); ++i) {
    if (i) body += ',';
    body += "{\"ssid\":\"";
    body += jsonEscaped(scannedNetworks[i].ssid);
    body += "\",\"rssi\":";
    body += String(scannedNetworks[i].rssi);
    body += ",\"secure\":";
    body += scannedNetworks[i].secure ? "true" : "false";
    body += '}';
  }
  body += ']';
  request->send(200, "application/json", body);
}

void registerRoutes() {
  if (routesRegistered) return;
  routesRegistered = true;
  AsyncWebServer &server = wsHttpServer();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", kSetupPage);
  });
  server.on("/networks", HTTP_GET, handleNetworksList);
  server.on("/setup", HTTP_POST, handleSetupSubmit);
  server.on("/setup/status", HTTP_GET, handleSetupStatus);
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });
}

void buildHandoffUrl(const WifiStatus &status, char *url, size_t urlSize) {
  // "<hostname>.local" rather than the raw IP - see WifiManager::poll(),
  // which starts mDNS right before this runs (both happen within the same
  // loop() iteration, via wifiManager.poll() called just before
  // setupPortalPoll()) - so it stays reachable across DHCP renewals too.
  char host[48];
  snprintf(host, sizeof(host), "%s.local", status.hostname);
  char token[kWifiTokenSize * 2 + 1];
  for (size_t i = 0; i < kWifiTokenSize; ++i) {
    snprintf(token + i * 2, 3, "%02x", pendingToken[i]);
  }
  snprintf(url, urlSize, "%s?host=%s&token=%s", kAppBaseUrl, host, token);
}

}  // namespace

void setupPortalBegin() {
  if (wifiManager.hasCredentials()) return;
  active = true;

  // Scan before the access point comes up (scanning and hosting an AP at
  // the same time is best avoided) - this briefly delays the AP appearing
  // by a couple of seconds, in exchange for the setup page being able to
  // show a tappable list of nearby networks.
  WiFi.mode(WIFI_STA);
  scanNearbyNetworks();

  WiFi.softAP(buildSetupSsid().c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  registerRoutes();
  // Starts the one shared AsyncWebServer (idempotent) - see wsHttpServer().
  wsServerBegin();
}

void setupPortalPoll() {
  if (!active) return;
  dnsServer.processNextRequest();

  const WifiStatus status = wifiManager.status();

  if (connectedAt == 0) {
    if (status.state != WIFI_CONNECTED) return;
    connectedAt = millis();

    snprintf(cachedHost, sizeof(cachedHost), "%s.local", status.hostname);
    if (kAppBaseUrl[0] != '\0' && havePendingToken) {
      buildHandoffUrl(status, cachedHandoffUrl, sizeof(cachedHandoffUrl));
      showWifiSetupQr(cachedHandoffUrl);
    } else {
      char ip[16];
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", status.ip[0], status.ip[1],
               status.ip[2], status.ip[3]);
      showWifiSetupInfo(ip, status.hostname);
    }
    return;
  }

  // Keep the access point up a little longer so the phone's still-open
  // setup page (polling /setup/status) has a chance to offer the
  // "Open Cyberclip" link before it disappears - see kApGracePeriodMs.
  if (millis() - connectedAt < kApGracePeriodMs) return;

  active = false;
  dnsServer.stop();
  WiFi.softAPdisconnect(/*wifioff=*/true);
}

}  // namespace cyberclip
