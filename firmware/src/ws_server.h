#pragma once

#include <ESPAsyncWebServer.h>
#include <stddef.h>
#include <stdint.h>

namespace cyberclip {

// Minimal WiFi transport for the CyberClip binary protocol: a WebSocket
// endpoint that carries the exact same framed packets as USB serial.
//
// Only one WebSocket client is accepted at a time. Immediately after
// connecting, a client's first binary message must be exactly its 16-byte
// pairing token (see wifi_manager.h); on success the connection is
// authorized and every later message's raw bytes are queued for the main
// loop to feed into a FrameParser, exactly like bytes read from Serial.
// This keeps all protocol parsing/handling on the single main-loop task,
// since AsyncWebServer's callbacks run on a different task.
//
// wsServerBegin() must only be called after WiFi.mode(WIFI_STA) - starting
// the underlying TCP listener before LWIP's tcpip task exists (i.e. while
// WiFi is still off) crashes with a lwIP "Invalid mbox" assertion. It is
// idempotent, so WifiManager::connectIfNeeded() (its only caller) can call
// it on every reconnect attempt. wsServerPoll()/wsReadByte() are always
// safe to call, even before the server has started.
void wsServerBegin();
void wsServerPoll();

// The single shared HTTP/WebSocket server, bound to port 80. Only one
// AsyncWebServer can bind that port, so setup_portal.cpp registers its own
// captive-portal routes on this same instance rather than creating another.
AsyncWebServer &wsHttpServer();

// Pops the next queued inbound byte into *out. Returns false if empty.
bool wsReadByte(uint8_t *out);

// Sends one complete reply frame (header + payload + CRC trailer) to the
// currently authorized client. No-op if there is none.
void wsSendFrame(const uint8_t *data, size_t length);

}  // namespace cyberclip
