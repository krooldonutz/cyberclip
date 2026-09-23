#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cyberclip {

// Minimal WiFi transport for the CyberClip binary protocol: a WebSocket
// endpoint that carries the exact same framed packets as USB serial.
//
// Only one WebSocket client is accepted at a time. Station-side clients must
// send the 16-byte pairing token as their first binary message. AP-side
// clients are identified from the TCP socket's local endpoint (192.168.4.1)
// and authorize with an empty first message. Trust is never based on headers
// or a client-supplied host.
// This keeps all protocol parsing/handling on the single main-loop task,
// since AsyncWebServer's callbacks run on a different task.
//
// wsServerBegin() must only be called after STA or AP networking has been
// enabled. Starting the listener before LWIP's tcpip task exists crashes with
// an "Invalid mbox" assertion. It is idempotent; wsServerPoll()/wsReadByte()
// are safe even before the server has started.
void wsServerBegin();
void wsServerPoll();

// Pops the next queued inbound byte into *out. Returns false if empty.
bool wsReadByte(uint8_t *out);

// Sends one complete reply frame (header + payload + CRC trailer) to the
// currently authorized client. No-op if there is none.
void wsSendFrame(const uint8_t *data, size_t length);

// Used to defer automatic AP shutdown while its authorized client is active
// or an AP-side HTTP asset transfer has run recently.
bool wsHasActiveApClientOrTransfer();

}  // namespace cyberclip
