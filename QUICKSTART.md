# Quick Start Guide - 5 Minute Setup

## Option A: Python CLI (Fastest ⚡)

Perfect if you just want to send images quickly from command line.

### 1. Install Python Dependencies
```bash
pip install pyserial pillow imageio
```

### 2. Upload Arduino Code
- Open Arduino IDE
- File → New → Copy code from `arduino_sketch/matrix_display.ino`
- Install `Adafruit NeoPixel` library
- Select your ESP32/ESP8266 board
- Upload

### 3. Send Images!
```bash
# List ports to find your ESP
python esp_control.py --list-ports

# Display image (replace COM3 with your port)
python esp_control.py -p COM3 -i myimage.jpg

# Display GIF animation
python esp_control.py -p COM3 -g animation.gif --delay 100

# Auto-detect ESP and send image
python esp_control.py -i myimage.jpg
```

**Done!** Your image should appear on the matrix.

---

## Option B: Web Dashboard (Best UX 🌐)

Interactive web interface with real-time preview.

### 1. Setup Same Arduino Code
(Same as Option A step 2)

### 2. Install Node.js
- Download from https://nodejs.org/
- Choose LTS version
- Install with default settings

### 3. Run the Server
```bash
# Navigate to this folder
cd esp_image_display

# Install Node packages
npm install

# Start server
npm start
```

### 4. Open Web Interface
- Open browser: http://localhost:3000
- Select COM port → Click Connect
- Upload image/GIF → Click Display
- Done!

---

## Option C: Bluetooth (Wireless 📱)

**Note:** Only works with ESP32 (not ESP8266)

### 1. Arduino Setup (Same, but enable Bluetooth)

In `matrix_display.ino`, ensure:
```cpp
#define USE_BLUETOOTH 1    // Enable this
```

### 2. Pair with Computer

**Windows:**
- Settings → Devices → Add Bluetooth device
- Find "ESP-Display"
- Pair (PIN: usually 1234 or 0000)

**Linux/Mac:**
- Pair through system Bluetooth settings
- Note the device MAC address

### 3. Find Bluetooth Port

After pairing, you'll get a COM port or device name:
- **Windows:** Look in Device Manager for "ESP-Display"
- **Linux:** `bluetoothctl` shows the device
- **Mac:** Usually `/dev/tty.ESP-Display-SPP`

### 4. Connect via Python or Web
```bash
# Same commands, but use Bluetooth port
python esp_control.py -p COM5 -i image.jpg

# Or use the web dashboard with the Bluetooth COM port
```

---

## Troubleshooting Quick Fixes

| Problem | Solution |
|---------|----------|
| "Port not found" | Run `python esp_control.py --list-ports` to see available ports |
| "Permission denied" | Windows: Run as Administrator, Linux: Use `sudo` or add user to `dialout` group |
| USB driver error | Download CH340 or CP2102 driver for your ESP board |
| Image appears corrupted | Verify matrix dimensions: `python esp_control.py -p COM3 -i image.jpg -w 32 -H 8` |
| Nothing displays | Check LED connections, verify GPIO pin = 5 in Arduino code |
| Bluetooth won't pair | Reset ESP: Press RST button, try pairing again |

---

## Next Steps

- 📖 Read full [README.md](README.md) for advanced features
- 🎨 Create custom images for your matrix size
- 🔄 Combine multiple GIFs
- 🌐 Integrate with other systems via API

---

## Example Workflow

```bash
# Step 1: List available ports
python esp_control.py --list-ports
# Output: COM3: USB CH340/CH341

# Step 2: Test connection
python esp_control.py -p COM3 --test
# Output: ✓ Connected...

# Step 3: Display image
python esp_control.py -p COM3 -i sunset.jpg

# Step 4: Display animated GIF
python esp_control.py -p COM3 -g loading.gif --delay 100

# Step 5: Custom size matrix
python esp_control.py -p COM3 -i image.jpg -w 64 -H 16
```

---

## Common Image Preparations

**For best results:**
- Image size: matching your matrix (32x8, 64x16, etc.)
- Format: PNG or JPG
- Color: Any - automatically converts to grayscale
- Contrast: Higher contrast = better details

**Create resized image (macOS/Linux):**
```bash
# Using ImageMagick
convert input.jpg -resize 32x8 output.jpg

# Using ffmpeg
ffmpeg -i input.jpg -vf scale=32:8 output.jpg
```

**Extract frames from video (create GIF):**
```bash
# Using ffmpeg
ffmpeg -i video.mp4 -vf scale=32:8 -r 10 animation.gif
```

---

**You're ready to go!** 🚀 Start with Option A (Python CLI) if unsure - it's the simplest!
