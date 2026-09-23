# Cyberclip

Cyberclip is a VIA-style web controller for the **ideaspark ESP32 development board with an integrated 1.9-inch 170×320 ST7789 TFT**. It processes images and GIFs in the browser, compresses each display frame as JPEG, and sends it directly to the board over USB with the Web Serial API, or over your own local WiFi network.

There is no application server, Python CLI, cloud relay, or Bluetooth pairing. WiFi is optional and local-only: the board can join your existing network or host its own WPA2-protected hotspot and phone upload page. WiFi and hotspot settings are provisioned over USB (see [WiFi control](#wifi-control) below). Media processing runs in the connected browser using the same image, GIF, and protocol modules in the desktop and hosted builds. Firmware flashing remains USB-only. The ESP32 receives validated frames, decodes them, and draws them to its display. When **Keep media on device** is enabled, it also stores the image or GIF in flash and resumes it without a USB data connection.

## Supported hardware

- ideaspark ESP32 development board
- ESP32-WROOM-32-class module with 16 MB flash
- Integrated 1.9-inch, 170×320 ST7789 TFT
- CH340 USB-to-serial interface over USB Type-C

The current board profile uses the seller-reported display connections:

| ST7789 signal | ESP32 pin |
|---|---:|
| MOSI | GPIO23 |
| SCLK | GPIO18 |
| CS | GPIO15 |
| DC | GPIO2 |
| Reset | GPIO4 |
| Backlight | GPIO32 |

Board revisions can differ. The current profile uses RGB panel order (`kRgbOrder = false`). Verify these pins, the 35-pixel horizontal panel offset, inversion, and RGB/BGR order against the example firmware supplied with your board before relying on the display.

## How it works

```text
Image or GIF
    │
    ▼
Chrome / Edge PWA
  - decode and composite media
  - resize to 170×320 or 320×170
  - preview and JPEG encode
  - schedule GIF frames
    │
    │ Web Serial at 921600 baud, or a local WebSocket over WiFi
    ▼
ESP32 firmware
  - validate framed packets and CRC
  - assemble bounded JPEG chunks
  - decode JPEG to RGB565 scanlines
  - draw to the ST7789
  - optionally persist and replay from flash
```

## Requirements

### Using the web app

- Current desktop Google Chrome or Microsoft Edge
- HTTPS hosting or `localhost`
- A data-capable USB Type-C cable
- A CH340 driver if your operating system does not already provide one
- Cyberclip firmware flashed to the board

Web Serial is not currently supported by Firefox or Safari. Connecting always requires a user gesture and browser permission; Cyberclip cannot silently access serial devices.

### Developing the web app

- Node.js 18 or newer
- npm

### Building the firmware

Create a versioned, merged firmware image for the web installer with one
PowerShell command from the repository root:

```powershell
.\scripts\build-firmware.ps1 <major>.<minor>.<patch>
```

Python 3 is required. The script installs
[PlatformIO Core](https://platformio.org/) with pip when needed, builds the
firmware, merges all ESP32 boot components, updates the firmware manifest and
service-worker cache, and removes the previously published binary. The version
argument must be newer than the version in `public/firmware/manifest.json`.

The firmware dependencies are pinned in `firmware/platformio.ini`.

The web installer uses the prebuilt firmware image referenced by `public/firmware/manifest.json` and validates its metadata before flashing. It writes the bootloader, partition table, and application image without erasing the LittleFS media partition.

## Quick start

1. Connect the board using a data-capable USB cable.
2. Open the Cyberclip web app in desktop Chrome or Edge.
3. Select **Install firmware**, choose the CH340 serial device, and keep it connected until the board restarts.
4. Select **Connect** and approve access if the browser prompts again.
5. Wait for Cyberclip to verify the firmware and display capabilities.
6. Choose an image or GIF, adjust its fit/rotation/quality, then select **Display image** or **Play GIF**.

See [QUICKSTART.md](QUICKSTART.md) for build and flashing commands.

## Local development

```powershell
npm install
npm run dev
```

Open the HTTPS/localhost address printed by Vite. Production assets are generated with:

```powershell
npm run build
npm run preview
```

The deployable static site is written to `dist/`. Deploy it at the origin root on an HTTPS host. The service worker caches same-origin application assets after first use so the installed app can launch offline; serial data and selected media are never cached.

## Media behavior

- Input formats: JPEG, PNG, WebP, and GIF
- Maximum input size: 20 MB
- Maximum GIF length: 255 decoded frames
- Output: full-color JPEG sized to the connected display and selected rotation
- Fit modes: contain, cover, and stretch
- Device frame limit: advertised during handshake; currently 128 KiB
- Persistent media capacity: advertised by firmware from the onboard flash filesystem

If a JPEG exceeds the device limit, Cyberclip lowers quality to a bounded minimum. It reports an error instead of sending a frame that still does not fit.

GIFs are decoded and composited in the browser. Each frame is resized, JPEG-encoded, transferred, and acknowledged. With persistence enabled, the ESP32 shows upload progress instead of rendering incomplete media; after the frames and delays are committed atomically to flash, it takes over playback. Cancelled or failed uploads restore the previously saved media. Without persistence, the browser continues scheduling frames and playback stops when USB disconnects.

Source GIF timing is preserved per frame at the format's 10 ms resolution unless a GIF delay override is set. Playback uses cumulative deadlines and skips frames whose display window has already expired, preserving the animation's original speed when encoding, transfer, or display decoding cannot sustain every frame.

Persisted images and GIFs survive reset and power loss. The board still needs power: disconnecting USB also turns the display off unless the ESP32 is powered from another suitable source.

Press the board's **BOOT** button during normal operation to enter deep sleep and turn off the display. Press **BOOT** again to wake the board and resume persisted media. Deep sleep minimizes consumption but does not physically disconnect power. The **EN/RESET** button remains a hardware reset, and holding BOOT while resetting or connecting power still enters the ESP32 firmware-download mode.

Hold **BOOT** for about two seconds during normal operation to toggle the hotspot between **Off** and its last enabled mode. A short press keeps the existing sleep behavior. The display shows which hotspot mode was selected.

## USB protocol

Every packet uses little-endian numeric fields:

```text
magic "CC" | version u8 | command u8 | sequence u16 |
payload length u32 | payload | CRC-16/CCITT-FALSE u16
```

The CRC covers bytes from `version` through the end of `payload`. Frames are transferred with begin, contiguous chunk, and commit commands. The ESP32 only draws a JPEG after the complete frame has passed length, sequence, CRC, dimension, and decode validation. Playlist commands stage one image or up to 255 GIF frames in the inactive flash generation, then atomically activate it so an interrupted upload does not destroy the previously saved media.

The handshake reports:

- device name and firmware version;
- native display dimensions;
- valid rotations;
- codec support;
- maximum packet chunk;
- maximum compressed frame size.

See [ARCHITECTURE.md](ARCHITECTURE.md) and `firmware/protocol.h` for the complete command/error definitions.

## WiFi control

WiFi is an additional, optional transport alongside USB - USB keeps working exactly as before. It is strictly local-network: the board joins your own WiFi and the browser talks to it directly at its local IP, with no cloud relay, no external server, and no background access.

1. Connect over USB first, as above.
2. Select **Set up WiFi**, and enter your network's name (SSID) and password. The web app sends these to the board over the existing USB connection.
3. The board joins your network and reports its local IP address (and an `<name>.local` hostname) back over USB. On the very first pairing, it also issues a random 16-byte pairing token, which the web app stores in this browser only.
4. Enter that IP address (or hostname) under **Connect over WiFi** and select **Connect over WiFi**. From then on, image/GIF upload, playlists, brightness, and clear/status all work the same way as over USB.

The pairing token gates the WiFi connection so other devices on your network cannot control the display or read its status without it. Firmware updates remain USB-only. Select **Forget WiFi** over USB or an active LAN connection to have the board forget its network and pairing token; set up WiFi again afterward to issue a new one (for example, to pair a different browser with the same board).

### ESP32-hosted phone page

Connect to the board over USB and configure **Device hotspot** in the desktop app. Choose a mode and set a device-specific WPA2 password of 8-63 characters:

- **Off** never starts the hotspot.
- **Automatic fallback** starts it when home WiFi is not configured or cannot connect, and stops it after home WiFi recovers.
- **Always on** keeps the hotspot available while the board may also remain connected to home WiFi.

When the hotspot is running, join the displayed `Cyberclip-......` network from a phone and open `http://192.168.4.1`. The hosted page supports image/GIF selection, exact preview and JPEG processing, fit, rotation, background, quality, backlight, timing, looping, persistence, clearing, and removing stored media. It intentionally contains no firmware installer or network setup controls.

The hotspot's WPA2 password is the access boundary for its self-hosted page. A WebSocket is accepted without the LAN pairing token only when the firmware observes that it arrived through the AP interface. Home-network WebSockets still require the random token issued during USB pairing. The password is stored on the device and is never returned to either web UI.

## Troubleshooting

### No serial device appears

- Use Chrome or Edge on desktop.
- Confirm the page is served through HTTPS or localhost.
- Try a different USB cable; many charging cables do not carry data.
- Install the CH340 driver from the chip vendor if the device is absent from Device Manager.
- Close Arduino Serial Monitor, PlatformIO Monitor, and other applications holding the COM port.

### Connection opens but identification fails

- Flash the Cyberclip firmware, not the original matrix/NeoPixel firmware.
- Confirm both browser and firmware use 921600 baud.
- Press the board reset button and reconnect.
- Check the activity log for protocol-version or timeout details.

### Image is shifted, mirrored, or has incorrect colors

The physical board profile needs adjustment. Verify these constants in `firmware/matrix_display.ino`:

- `kOffsetX` and `kOffsetY`;
- `kInvert`;
- `kRgbOrder`;
- the six TFT/backlight pins.

If red and blue are swapped, toggle `kRgbOrder`, rebuild, and reflash the firmware.

### GIF playback is slower than expected

Every frame must be encoded, sent, acknowledged, and decoded. Reduce JPEG quality, use simpler source frames, or increase the GIF delay. Cyberclip intentionally does not buffer an entire GIF in ESP32 RAM.

### Saved media does not appear after unplugging USB

USB supplies both data and power. Use another suitable power source after disconnecting USB. Confirm **Keep media on device** was enabled and wait for **Saved on device** or **Saved and playing on device** before disconnecting.

### Transfer stops after disconnecting the cable

Reconnect the board, press **Connect**, and approve the port again if prompted. Incomplete frames are discarded and the last valid image remains on screen.

## Repository layout

```text
src/
  app.js             Browser state and UI
  serial.js          Web Serial lifecycle
  wifiTransport.js   WebSocket lifecycle (mirrors serial.js) for WiFi control
  protocol.js        Framing, CRC, commands, payload helpers
  images.js          Resize, fit, preview, JPEG encoding
  gifs.js            GIF validation, compositing, timing
public/
  index.html
  manifest.json
  service-worker.js
firmware/
  matrix_display.ino
  protocol.h
  platformio.ini
  src/
    wifi_manager.h/.cpp   WiFi credentials, pairing token, connection lifecycle
    ws_server.h/.cpp      WebSocket transport carrying the same binary protocol
  test/
```
