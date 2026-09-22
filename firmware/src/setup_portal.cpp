#include "setup_portal.h"

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "../protocol.h"
#include "display_hooks.h"
#include "wifi_manager.h"
#include "ws_server.h"

namespace cyberclip {

namespace {

DNSServer dnsServer;

bool active = false;
bool routesRegistered = false;
uint8_t pendingToken[kWifiTokenSize] = {};
bool havePendingToken = false;

String buildSetupSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "Cyberclip-Setup-%02X%02X", mac[4], mac[5]);
  return String(ssid);
}

const char kSetupPage[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cyberclip setup</title>
<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}
input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;font-size:16px}
button{width:100%;padding:12px;font-size:16px;margin-top:12px}</style></head>
<body><h1>Connect Cyberclip to WiFi</h1>
<form method="POST" action="/setup">
<label>Network name (SSID)<input name="ssid" required maxlength="32"></label>
<label>Password<input name="password" type="password" maxlength="64"></label>
<button type="submit">Connect</button>
</form></body></html>
)HTML";

const char kConnectingPage[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title>
<style>body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px;text-align:center}</style>
</head><body><h1 id="msg">Connecting&hellip;</h1>
<p id="detail">Keep this page open.</p>
<script>
async function poll() {
  try {
    const response = await fetch('/setup/status');
    const status = await response.json();
    if (status.state === 2) {
      document.getElementById('msg').textContent = 'Connected!';
      document.getElementById('detail').textContent =
        "Look at the board's screen for a QR code, or switch back to your usual WiFi/data and open the Cyberclip app.";
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
  char body[32];
  snprintf(body, sizeof(body), "{\"state\":%u}", wifiManager.status().state);
  request->send(200, "application/json", body);
}

void registerRoutes() {
  if (routesRegistered) return;
  routesRegistered = true;
  AsyncWebServer &server = wsHttpServer();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", kSetupPage);
  });
  server.on("/setup", HTTP_POST, handleSetupSubmit);
  server.on("/setup/status", HTTP_GET, handleSetupStatus);
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });
}

void buildHandoffUrl(const WifiStatus &status, char *url, size_t urlSize) {
  char ip[16];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", status.ip[0], status.ip[1],
           status.ip[2], status.ip[3]);
  char token[kWifiTokenSize * 2 + 1];
  for (size_t i = 0; i < kWifiTokenSize; ++i) {
    snprintf(token + i * 2, 3, "%02x", pendingToken[i]);
  }
  snprintf(url, urlSize, "%s?host=%s&token=%s", kAppBaseUrl, ip, token);
}

}  // namespace

void setupPortalBegin() {
  if (wifiManager.hasCredentials()) return;
  active = true;

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
  if (status.state != WIFI_CONNECTED) return;

  active = false;
  dnsServer.stop();
  WiFi.softAPdisconnect(/*wifioff=*/true);

  if (kAppBaseUrl[0] != '\0' && havePendingToken) {
    char url[192];
    buildHandoffUrl(status, url, sizeof(url));
    showWifiSetupQr(url);
  } else {
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", status.ip[0], status.ip[1],
             status.ip[2], status.ip[3]);
    showWifiSetupInfo(ip, status.hostname);
  }
}

}  // namespace cyberclip
