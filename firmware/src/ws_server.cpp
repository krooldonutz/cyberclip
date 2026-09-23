#include "ws_server.h"

#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <string.h>

#include "../generated/device_web.h"
#include "../protocol.h"
#include "wifi_manager.h"

namespace cyberclip {

namespace {
constexpr size_t kMaxMessageSize = kMaxWirePayload + 10 + 2;
constexpr size_t kRxQueueSize = 8192;
constexpr uint32_t kApHttpTransferGraceMs = 10000;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

AsyncWebSocketClient *activeClient = nullptr;
bool activeClientAuthorized = false;
bool activeClientIsAp = false;
volatile uint32_t lastApHttpActivityAt = 0;
uint8_t messageBuffer[kMaxMessageSize];

portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t rxQueue[kRxQueueSize];
size_t rxHead = 0;
size_t rxTail = 0;
size_t rxCount = 0;

void enqueueBytes(const uint8_t *data, size_t length) {
  portENTER_CRITICAL(&rxMux);
  for (size_t i = 0; i < length && rxCount < kRxQueueSize; ++i) {
    rxQueue[rxTail] = data[i];
    rxTail = (rxTail + 1) % kRxQueueSize;
    ++rxCount;
  }
  portEXIT_CRITICAL(&rxMux);
}

void handleClose(AsyncWebSocketClient *client) {
  client->close();
  if (activeClient == client) {
    activeClient = nullptr;
    activeClientAuthorized = false;
    activeClientIsAp = false;
  }
}

void handleMessage(AsyncWebSocketClient *client, const uint8_t *data,
                   size_t length) {
  if (!activeClientAuthorized) {
    if ((activeClientIsAp && length == 0) ||
        (!activeClientIsAp && length == kWifiTokenSize &&
         wifiManager.checkToken(data, length))) {
      activeClientAuthorized = true;
    } else {
      handleClose(client);
    }
    return;
  }
  enqueueBytes(data, length);
}

bool requestArrivedOnAp(AsyncWebServerRequest *request) {
  return request->client()->localIP() == IPAddress(192, 168, 4, 1);
}

const EmbeddedWebFile *findWebFile(const String &url) {
  const String path = url == "/" ? "/index.html" : url;
  for (size_t i = 0; i < kDeviceWebFileCount; ++i) {
    if (path == kDeviceWebFiles[i].path) return &kDeviceWebFiles[i];
  }
  return nullptr;
}

void handleHttpRequest(AsyncWebServerRequest *request) {
  if (!requestArrivedOnAp(request)) {
    request->send(404);
    return;
  }
  lastApHttpActivityAt = millis();
  const EmbeddedWebFile *file = findWebFile(request->url());
  // Captive-portal probes use several arbitrary paths (some with extensions).
  // Every unknown AP-side request receives the app shell.
  if (!file) file = findWebFile("/index.html");
  if (!file) {
    request->send(404);
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse(
      200, file->contentType, file->data, file->size);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

void handleWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      if (activeClient != nullptr) {
        client->close();
        return;
      }
      activeClient = client;
      activeClientAuthorized = false;
      activeClientIsAp =
          client->client()->localIP() == WiFi.softAPIP() &&
          WiFi.softAPIP() == IPAddress(192, 168, 4, 1);
      break;

    case WS_EVT_DISCONNECT:
      if (activeClient == client) {
        activeClient = nullptr;
        activeClientAuthorized = false;
        activeClientIsAp = false;
      }
      break;

    case WS_EVT_DATA: {
      if (client != activeClient) break;
      auto *info = static_cast<AwsFrameInfo *>(arg);
      if (info->index + len > kMaxMessageSize) {
        handleClose(client);
        break;
      }
      memcpy(messageBuffer + info->index, data, len);
      if (!info->final) break;
      handleMessage(client, messageBuffer, info->index + len);
      break;
    }

    case WS_EVT_ERROR:
    case WS_EVT_PONG:
      break;
  }
}

}  // namespace

void wsServerBegin() {
  // Safe to call every time networking comes up. WifiManager first enables
  // STA or AP so LWIP's tcpip task exists before this listener is started.
  static bool started = false;
  if (started) return;
  started = true;

  ws.onEvent(handleWsEvent);
  server.addHandler(&ws);
  server.onNotFound(handleHttpRequest);
  server.begin();
}

bool wsHasActiveApClientOrTransfer() {
  if (activeClient != nullptr && activeClientAuthorized &&
      activeClientIsAp && activeClient->status() == WS_CONNECTED) {
    return true;
  }
  const uint32_t activityAt = lastApHttpActivityAt;
  return activityAt != 0 && millis() - activityAt < kApHttpTransferGraceMs;
}

void wsServerPoll() { ws.cleanupClients(); }

bool wsReadByte(uint8_t *out) {
  bool hasByte = false;
  portENTER_CRITICAL(&rxMux);
  if (rxCount > 0) {
    *out = rxQueue[rxHead];
    rxHead = (rxHead + 1) % kRxQueueSize;
    --rxCount;
    hasByte = true;
  }
  portEXIT_CRITICAL(&rxMux);
  return hasByte;
}

void wsSendFrame(const uint8_t *data, size_t length) {
  if (activeClient != nullptr && activeClientAuthorized &&
      activeClient->status() == WS_CONNECTED) {
    activeClient->binary(data, length);
  }
}

}  // namespace cyberclip
