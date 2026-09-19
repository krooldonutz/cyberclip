# Cyberclip quick start

## 1. Install the CH340 driver

Connect the ideaspark ESP32 board over USB Type-C. If it does not appear as a serial/COM device, install the CH340 driver for your operating system and reconnect it.

Use a data-capable cable. A charge-only cable will power the display but cannot flash firmware or carry images.

## 2. Flash the firmware

Install PlatformIO, open the repository, and run:

```powershell
pio run -d firmware
pio run -d firmware -t upload
```

The firmware targets an ESP32 Dev Module, uses the integrated ST7789 at 170×320, and communicates at 921600 baud. PlatformIO downloads the pinned display and JPEG libraries automatically.

If uploading at 921600 is unreliable on your computer, lower only `upload_speed` in `firmware/platformio.ini`. Do not change `monitor_speed` or the firmware/browser transport rate.

## 3. Start the web app locally

Install Node.js 18 or newer, then run:

```powershell
npm install
npm run dev
```

Open the localhost URL printed by Vite in desktop Chrome or Edge.

## 4. Connect and display media

1. Select **Connect**.
2. Choose the CH340 USB serial device.
3. Wait for the app to show the firmware version and 170×320 display.
4. Choose a JPEG, PNG, WebP, or GIF up to 20 MB.
5. Select fit, rotation, background, JPEG quality, and backlight.
6. Select **Display image** or **Play GIF**.

Leave **Keep media on device** enabled to store the result in onboard flash. A saved image is restored after restart; a saved GIF plays from the ESP32 without the browser or USB data connection. GIFs are limited to 255 frames. **Stop** cancels playback and any incomplete transfer, while **Remove saved media** erases the persistent copy.

The ESP32 must remain powered. If USB is its only power source, unplugging USB turns the board and display off.

## 5. Build the installable app

```powershell
npm run build
npm run preview
```

Deploy `dist/` at the root of an HTTPS origin. Open it once while online, then use the browser's install option. The installed app shell works offline and continues to communicate directly with the ESP32 over USB.

## First-display checks

The board listing reports this integrated display wiring:

```text
MOSI  GPIO23
SCLK  GPIO18
CS    GPIO15
DC    GPIO2
RST   GPIO4
BL    GPIO32
```

If the backlight turns on but graphics are offset or incorrectly colored, adjust the board constants near the top of `firmware/matrix_display.ino`. The current profile assumes an ST7789 240×320 controller with a centered 170-pixel window (`kOffsetX = 35`), inversion enabled, and RGB panel order (`kRgbOrder = false`). If red and blue are swapped on a different board revision, toggle `kRgbOrder`, rebuild, and reflash.
