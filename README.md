# ESP Matrix Display Controller

A complete system for displaying images and animated GIFs on an LED matrix connected to an ESP32/ESP8266 device. Includes a web frontend, Node.js backend, Python CLI, and Arduino firmware.

## Features

✨ **Multiple Interfaces**
- Web dashboard with real-time preview
- Python command-line tool
- REST API for integration

📡 **Connectivity**
- USB Serial (immediate connection)
- Bluetooth (wireless control on ESP32)
- Auto-detection of ESP devices

🎨 **Image Support**
- Static images (PNG, JPEG, BMP)
- Animated GIFs with frame-by-frame control
- Automatic resize and contrast adjustment
- Grayscale conversion with threshold

⚙️ **Customizable**
- Configurable matrix dimensions
- Adjustable animation frame delays
- Brightness control
- Custom communication protocol

## System Requirements

### Hardware
- ESP32 or ESP8266 microcontroller
- WS2812B RGB LED strip or similar addressable LEDs
- USB cable (for initial programming & serial connection)

### Software
- **For Node.js backend:**
  - Node.js 14+ 
  - npm
  - Serial driver (CH340, CP2102, etc. depending on your ESP board)

- **For Python CLI:**
  - Python 3.7+
  - pip

## Installation

### 1. ESP Arduino Firmware

1. Open Arduino IDE
2. Install board support:
   - Go to Tools → Board Manager
   - Search for "esp32" or "esp8266"
   - Install the board package

3. Install required libraries:
   - Sketch → Include Library → Manage Libraries
   - Search for and install:
     - `Adafruit NeoPixel` (for WS2812B LEDs)
     - `BluetoothSerial` (if using Bluetooth on ESP32)

4. Copy the code from `arduino_sketch/matrix_display.ino`

5. Configure for your setup:
```cpp
#define MATRIX_WIDTH 32      // Your matrix width
#define MATRIX_HEIGHT 8      // Your matrix height
#define PIXEL_PIN 5          // GPIO pin where data line connects
#define USE_BLUETOOTH 1      // Enable Bluetooth (ESP32 only)
```

6. Select your board:
   - Tools → Board → ESP32 Dev Module (or your specific board)
   - Select COM port
   - Click Upload

### 2. Node.js Web Interface (Optional)

```bash
# Clone or download this folder
cd esp_image_display

# Install dependencies
npm install

# Start the server
npm start

# The web interface will be available at http://localhost:3000
```

### 3. Python CLI Tool

```bash
# Install dependencies
pip install pyserial pillow imageio

# List available ports
python esp_control.py --list-ports

# Test connection
python esp_control.py -p COM3 --test

# Display an image
python esp_control.py -p COM3 -i myimage.jpg

# Display animated GIF
python esp_control.py -p COM3 -g animation.gif --delay 100
```

## Usage

### Via Web Dashboard

1. Start the Node.js server: `npm start`
2. Open http://localhost:3000 in your browser
3. Select serial port and click Connect
4. Upload an image or GIF
5. Click "Display Image" or "Display GIF Animation"

### Via Python CLI

**Display a static image:**
```bash
python esp_control.py -p COM3 -i photo.jpg
```

**Display animated GIF:**
```bash
python esp_control.py -p COM3 -g animation.gif --delay 100
```

**With custom matrix size:**
```bash
python esp_control.py -p COM3 -i image.jpg -w 64 -H 16
```

**Auto-detect ESP port:**
```bash
python esp_control.py -i image.jpg
```

### Via REST API

The Node.js backend exposes these endpoints:

**List serial ports:**
```bash
curl http://localhost:3000/api/ports
```

**Connect to device:**
```bash
curl -X POST http://localhost:3000/api/connect \
  -H "Content-Type: application/json" \
  -d '{"portPath": "COM3", "baudRate": 115200}'
```

**Display image:**
```bash
curl -X POST http://localhost:3000/api/display-image \
  -H "Content-Type: application/json" \
  -d '{"imagePath": "/path/to/image.jpg"}'
```

**Display GIF:**
```bash
curl -X POST http://localhost:3000/api/display-gif \
  -H "Content-Type: application/json" \
  -d '{"gifPath": "/path/to/animation.gif", "delayMs": 100}'
```

## Communication Protocol

The system uses a binary protocol for efficient data transmission:

### Display Image Packet
```
[START: 0xFF][CMD: 0x01][WIDTH][HEIGHT][PIXEL_DATA...]
```
- START: 0xFF (packet start marker)
- CMD: 0x01 (display image command)
- WIDTH: Matrix width in pixels
- HEIGHT: Matrix height in pixels
- PIXEL_DATA: Grayscale values (0-255) for each pixel

Example (8x8 matrix, all white):
```
0xFF 0x01 0x08 0x08 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF ...
```

### Animation Start Packet
```
[START: 0xFF][CMD: 0x02][FRAME_COUNT][DELAY_H][DELAY_L]
```
- START: 0xFF
- CMD: 0x02 (animation command)
- FRAME_COUNT: Number of frames
- DELAY_H: Delay upper byte
- DELAY_L: Delay lower byte
- Delay in milliseconds = (DELAY_H << 8) | DELAY_L

## Bluetooth Setup (ESP32)

The system automatically supports Bluetooth Classic on ESP32 devices.

**Pairing on Windows:**
1. Settings → Devices → Add Bluetooth or other device
2. Select device named "ESP-Display"
3. Pin code (if prompted): 1234 or 0000

**Using Python with Bluetooth:**
```bash
# Install additional package
pip install pybluez

# Modify esp_control.py to use Bluetooth instead:
# Change: port = "COM3"
# To: port = "ESP-Display"  # or MAC address like "AA:BB:CC:DD:EE:FF"
```

## Troubleshooting

### Can't connect to ESP
1. Check USB cable and driver installation
2. Verify COM port number: `python esp_control.py --list-ports`
3. Reset ESP device (press RST button)
4. Try different baud rate: `python esp_control.py -p COM3 -b 9600 --test`

### Image doesn't display
1. Check matrix size is correct
2. Verify LED connection to GPIO pin (default: GPIO5)
3. Test with simple colors in Arduino IDE:
```cpp
pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Red
pixels.show();
```

### Garbled image received
1. Verify baud rate matches ESP: default 115200
2. Check USB cable quality (use short, high-quality cable)
3. Try reducing matrix size or delay between frames

### Bluetooth not working
1. Confirm ESP32 board (not ESP8266, which doesn't have BLE)
2. Re-check Bluetooth pairing
3. Add these lines to Arduino sketch if needed:
```cpp
SerialBT.begin(BLUETOOTH_NAME);  // This starts Bluetooth
```

## Customization

### Change LED Matrix Dimensions

**Arduino:**
```cpp
#define MATRIX_WIDTH 64
#define MATRIX_HEIGHT 16
#define NUMPIXELS (MATRIX_WIDTH * MATRIX_HEIGHT)
```

**Python:**
```bash
python esp_control.py -p COM3 -i image.jpg -w 64 -H 16
```

**Web Dashboard:**
Settings panel → Width/Height → Apply Settings

### Use Different LED Strip Type

Update Arduino code:
```cpp
// For WS2811 RGB LEDs:
Adafruit_NeoPixel pixels(NUMPIXELS, PIXEL_PIN, NEO_RGB + NEO_KHZ800);

// For APA102 (DotStar):
#include <Adafruit_DotStar.h>
Adafruit_DotStar strip(NUMPIXELS, 2, 4, DOTSTAR_BRG);
```

### Adjust Image Processing

In `server.js` or `esp_control.py`, modify the threshold value:
```python
# Current: Convert to binary at threshold 127
matrix.append(brightness > 127 ? 255 : 0)

# For more detail (lower threshold):
matrix.append(brightness > 100 ? 255 : 0)

# For higher contrast (higher threshold):
matrix.append(brightness > 200 ? 255 : 0)
```

## Performance Tips

- **Reduce frame delay** for smoother animation: `--delay 50`
- **Smaller matrix size** for faster updates
- **Use JPEG instead of PNG** for smaller file sizes
- **Pre-process GIFs** (reduce to fewer frames) for large displays
- **Use higher baud rate** if ESP supports it: `--baud 921600`

## Project Structure

```
esp_image_display/
├── arduino_sketch/
│   └── matrix_display.ino          # ESP firmware
├── public/
│   └── index.html                  # Web dashboard
├── server.js                       # Node.js backend
├── esp_control.py                  # Python CLI tool
├── package.json                    # Node.js dependencies
└── README.md                       # This file
```

## Advanced: Creating Custom Firmware

To modify the protocol or add features:

1. Edit `arduino_sketch/matrix_display.ino`
2. Change command codes:
```cpp
const uint8_t CMD_DISPLAY_IMAGE = 0x01;
const uint8_t CMD_CUSTOM = 0x03;        // Add new command
```
3. Add handler function:
```cpp
case CMD_CUSTOM:
  handleCustomCommand();
  break;
```

## API Reference

### REST Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/ports` | List available serial ports |
| POST | `/api/connect` | Connect to ESP device |
| POST | `/api/disconnect` | Disconnect from ESP |
| POST | `/api/display-image` | Send image to display |
| POST | `/api/display-gif` | Send GIF animation |
| POST | `/api/config` | Update display settings |

### Python API

```python
from esp_control import ESPDisplayController

# Create controller
esp = ESPDisplayController(port="COM3", baudrate=115200)

# Connect
esp.connect()

# Display operations
esp.display_image("photo.jpg")
esp.display_gif("animation.gif", delay_ms=100)

# Custom matrix size
esp.matrix_width = 64
esp.matrix_height = 16

# Disconnect
esp.disconnect()
```

## License

MIT License - Feel free to use and modify for your projects!

## Support

For issues:
1. Check troubleshooting section
2. Verify hardware connections
3. Check serial monitor output in Arduino IDE
4. Open an issue with detailed information

## Future Enhancements

- [ ] Web-based image editor
- [ ] Real-time live feed from webcam
- [ ] Color matrix support
- [ ] Cloud storage integration
- [ ] Mobile app
- [ ] MQTT support for smart home integration
