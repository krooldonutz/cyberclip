# Cyberclip

Cyberclip is a VIA-style web controller for the **ideaspark ESP32 development board with an integrated 1.9-inch 170×320 ST7789 TFT**. It processes images and GIFs in the browser, compresses each display frame as JPEG, and sends it directly to the board over USB with the Web Serial API.

There is no application server, Python CLI, cloud relay, Wi-Fi transfer, or Bluetooth pairing. The ESP32 receives validated frames, decodes them, and draws them to its display. When **Keep media on device** is enabled, it also stores the image or GIF in flash and resumes it without a USB data connection.

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

Board revisions can differ. Verify these pins, the 35-pixel horizontal panel offset, inversion, and RGB/BGR order against the example firmware supplied with your board before relying on the display.

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
    │ Web Serial at 921600 baud
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

- [PlatformIO](https://platformio.org/) through VS Code or its CLI

The firmware dependencies are pinned in `firmware/platformio.ini`.

## Quick start

1. Flash `firmware/matrix_display.ino` using the PlatformIO configuration in `firmware/platformio.ini`.
2. Connect the board using a data-capable USB cable.
3. Open the Cyberclip web app in desktop Chrome or Edge.
4. Select **Connect**, choose the CH340 serial device, and approve access.
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

GIFs are decoded and composited in the browser. Each frame is resized, JPEG-encoded, transferred, and acknowledged. With persistence enabled, the frames and delays are committed atomically to flash and the ESP32 takes over playback. Without persistence, the browser continues scheduling frames and playback stops when USB disconnects.

Persisted images and GIFs survive reset and power loss. The board still needs power: disconnecting USB also turns the display off unless the ESP32 is powered from another suitable source.

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
  test/
```
