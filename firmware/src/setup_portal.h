#pragma once

namespace cyberclip {

// SoftAP + captive portal WiFi provisioning for phone-only setup, with no
// USB or computer involved at all. Only relevant when the device has never
// been paired (see WifiManager::hasCredentials()) - once real credentials
// exist, this never runs again unless they are cleared (CLEAR_WIFI_CREDENTIALS
// over USB or WiFi, or the "Forget WiFi" button in the web app).
//
// Starts an open, uniquely-named access point ("Cyberclip-Setup-XXXX",
// suffixed with two bytes of the device's MAC address) serving a single
// setup page at its own IP - most phones detect and open this automatically
// as a captive portal. Submitting the form there calls the exact same
// WifiManager::setCredentials() path USB pairing uses. Once the resulting
// WiFi connection succeeds, the access point is torn down and
// showWifiSetupQr()/showWifiSetupInfo() (display_hooks.h) renders the
// handoff on the device's own screen. A failed attempt leaves the access
// point up so the same phone can retry from the same page.
void setupPortalBegin();
void setupPortalPoll();

}  // namespace cyberclip
