# Cyberclip quick start

## 1. Connect the board

Connect the LilyGO T-Display-S3 over USB Type-C. It uses the ESP32-S3's native USB port, so no USB-to-serial driver is needed. A board without Cyberclip shows up as **USB JTAG/serial debug unit**; if it does not appear, hold **BOOT**, press **RST**, then release **BOOT**.

Use a data-capable cable. A charge-only cable will power the display but cannot flash firmware or carry images.

## 2. Flash the firmware

Install PlatformIO, open the repository, and run:

```powershell
pio run -d firmware
pio run -d firmware -t upload
```

The firmware targets the LilyGO T-Display-S3, uses the integrated ST7789 at 170×320, and communicates at 921600 baud. PlatformIO downloads the pinned display, JPEG, and Bluetooth libraries automatically.

## 3. Start the web app locally

Install Node.js 18 or newer, then run:

```powershell
npm install
npm run dev
```

Open the localhost URL printed by Vite in desktop Chrome or Edge.

## 4. Connect and display media

1. Select **Connect**.
2. Choose **Pixie Pixel Gear**.
3. Wait for the app to show the firmware version and 170×320 display.
4. Choose a JPEG, PNG, WebP, or GIF up to 20 MB.
5. Select fit, rotation, background, JPEG quality, and backlight.
6. Select **Display image** or **Play GIF**.

## 5. Upload from a phone

1. Connect through USB in the desktop app.
2. Under **Device hotspot**, select **Automatic fallback** or **Always on**.
3. Enter an 8-63 character WPA2 password and select **Save hotspot**.
4. Join the displayed `Cyberclip-......` WiFi network from your phone.
5. Open `http://192.168.4.1`, choose an image or GIF, adjust it, and upload it.

The phone performs image resizing, GIF compositing, JPEG encoding, and transfer scheduling. Firmware installation remains available only from the desktop app over USB.

A short BOOT press keeps the existing sleep/wake behavior. Hold BOOT for about two seconds to switch the hotspot between **Off** and its last enabled mode.

Leave **Keep media on device** enabled to store the result in onboard flash. A saved image is restored after restart; a saved GIF plays from the ESP32 without the browser or USB data connection. GIFs are limited to 255 frames. **Stop** cancels playback and any incomplete transfer, while **Remove saved media** erases the persistent copy.

The ESP32 must remain powered. If USB is its only power source, unplugging USB turns the board and display off.

## 6. Build the installable app

```powershell
npm run build
npm run preview
```

Deploy `dist/` at the root of an HTTPS origin. Open it once while online, then use the browser's install option. The installed app shell works offline and continues to communicate directly with the ESP32 over USB.

## First-display checks

The board's integrated display uses an 8-bit parallel bus:

```text
D0-D7   GPIO39, 40, 41, 42, 45, 46, 47, 48
WR/RD   GPIO8 / GPIO9
DC      GPIO7
CS      GPIO6
RST     GPIO5
BL      GPIO38
LCD_PWR GPIO15
```

If the backlight turns on but graphics are offset or incorrectly colored, adjust the board constants near the top of `firmware/matrix_display.ino`. The current profile assumes an ST7789 240×320 controller with a centered 170-pixel window (`kOffsetX = 35`), inversion enabled, and RGB panel order (`kRgbOrder = false`). If red and blue are swapped, toggle `kRgbOrder`, rebuild, and reflash.
