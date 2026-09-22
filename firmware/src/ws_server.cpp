#include "ws_server.h"

#include <ESPAsyncWebServer.h>
#include <string.h>

#include "../protocol.h"
#include "wifi_manager.h"

namespace cyberclip {

namespace {
constexpr size_t kMaxMessageSize = kMaxWirePayload + 10 + 2;
constexpr size_t kRxQueueSize = 8192;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

AsyncWebSocketClient *activeClient = nullptr;
bool activeClientAuthorized = false;
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
  }
}

void handleMessage(AsyncWebSocketClient *client, const uint8_t *data,
                   size_t length) {
  if (!activeClientAuthorized) {
    if (length == kWifiTokenSize && wifiManager.checkToken(data, length)) {
      activeClientAuthorized = true;
    } else {
      handleClose(client);
    }
    return;
  }
  enqueueBytes(data, length);
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
      break;

    case WS_EVT_DISCONNECT:
      if (activeClient == client) {
        activeClient = nullptr;
        activeClientAuthorized = false;
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
  ws.onEvent(handleWsEvent);
  server.addHandler(&ws);
  server.begin();
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
