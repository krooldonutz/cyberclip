#pragma once

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
void wsServerBegin();
void wsServerPoll();

// Pops the next queued inbound byte into *out. Returns false if empty.
bool wsReadByte(uint8_t *out);

// Sends one complete reply frame (header + payload + CRC trailer) to the
// currently authorized client. No-op if there is none.
void wsSendFrame(const uint8_t *data, size_t length);

}  // namespace cyberclip
