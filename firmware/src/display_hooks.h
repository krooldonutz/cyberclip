#pragma once

namespace cyberclip {

// Implemented in matrix_display.ino, which owns the display. Renders a QR
// code encoding `url` (the configured kAppBaseUrl plus ?host=&token=) so a
// phone can scan it to open the web app already pointed at this device.
// Called once, right after the setup portal's WiFi connection succeeds.
void showWifiSetupQr(const char *url);

// Fallback shown instead when no kAppBaseUrl is configured: the IP address
// and hostname as plain text, for typing into the app manually.
void showWifiSetupInfo(const char *ip, const char *hostname);

}  // namespace cyberclip
