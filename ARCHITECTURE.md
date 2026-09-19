# System Architecture & Technical Overview

## High-Level System Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                      YOUR WINDOWS PC                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐      ┌──────────────┐    ┌──────────────┐    │
│  │ Web Browser  │      │ Command Line │    │  REST API    │    │
│  │ Dashboard    │      │   (Python)   │    │  Clients     │    │
│  └──────┬───────┘      └──────┬───────┘    └──────┬───────┘    │
│         │                     │                    │            │
│         └─────────────────────┼────────────────────┘            │
│                               │                                 │
│                      ┌────────▼────────┐                        │
│                      │ Image Processing │                       │
│                      │   Engine         │                       │
│                      │                  │                       │
│                      │ • Sharp (resize) │                       │
│                      │ • PIL (convert)  │                       │
│                      │ • imageio (GIF)  │                       │
│                      └────────┬─────────┘                       │
│                               │                                 │
│                      ┌────────▼────────┐                        │
│                      │  Protocol Layer  │                       │
│                      │                  │                       │
│                      │ Binary Protocol: │                       │
│                      │ [0xFF][CMD][...] │                       │
│                      └────────┬─────────┘                       │
└───────────────────────────────┼────────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │                       │
         ┌──────────▼────────┐  ┌──────────▼────────┐
         │  Serial/USB       │  │  Bluetooth (BLE)  │
         │  ~230 kbps        │  │  ~250 kbps        │
         └──────────┬────────┘  └──────────┬────────┘
                    │                       │
                    └───────────┬───────────┘
                                │
         ┌──────────────────────▼──────────────────────┐
         │        ESP32/ESP8266 Microcontroller        │
         ├───────────────────────────────────────────┤
         │                                            │
         │  Serial/Bluetooth Receiver                 │
         │  │                                         │
         │  ├─▶ Parse incoming packets                │
         │  ├─▶ Extract pixel data                    │
         │  └─▶ Update display buffer                 │
         │                                            │
         │  Pixel Renderer                            │
         │  │                                         │
         │  ├─▶ Read from buffer                      │
         │  ├─▶ Convert brightness to color           │
         │  └─▶ Update LED driver                     │
         │                                            │
         │  LED Driver (NeoPixel library)             │
         │  │                                         │
         │  └─▶ Generate timing signals for LEDs      │
         └──────────────────────┬──────────────────────┘
                                │
                                │ GPIO5 (Data Line)
                                │
         ┌──────────────────────▼──────────────────────┐
         │  WS2812B LED Matrix Display                 │
         │  (32x8 or custom size)                     │
         │                                            │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         │  ████████████████████████████████          │
         └──────────────────────────────────────────────┘
```

## Data Flow

### 1. Image Upload
```
User selects image → Browser → Server receives file → Sharp processes
  ↓
Resize to matrix dimensions (32x8)
  ↓
Convert to grayscale
  ↓
Threshold to binary (0 or 255)
  ↓
Create binary packet [0xFF][0x01][32][8][DATA...]
  ↓
Send via Serial/Bluetooth to ESP
```

### 2. GIF Animation
```
User selects GIF → Server extracts frames → Process each frame:
  ↓
Send animation start command [0xFF][0x02][FRAMES][DELAY_H][DELAY_L]
  ↓
Loop through frames:
  - Create packet for frame
  - Send to ESP
  - Wait for frame delay
  - Move to next frame
```

## File Organization

```
esp_image_display/
│
├── 📱 FRONTEND LAYER
│   └── public/index.html
│       • Web dashboard UI
│       • Real-time port discovery
│       • Image preview
│       • Activity logging
│       • Socket.io connection
│
├── 🔧 BACKEND LAYER
│   ├── server.js
│   │   • Express HTTP server
│   │   • Serial port communication
│   │   • Image processing (Sharp)
│   │   • GIF frame extraction
│   │   • Binary protocol packet creation
│   │   • REST API endpoints
│   │   • WebSocket (Socket.io)
│   │
│   └── package.json
│       • Node.js dependencies
│
├── 🐍 PYTHON CLI TOOLS
│   ├── esp_control.py
│   │   • Command-line interface
│   │   • Image to matrix conversion (PIL)
│   │   • GIF frame extraction (imageio)
│   │   • Serial port auto-detection
│   │   • Cross-platform support
│   │
│   └── bluetooth_helper.py
│       • Bluetooth device discovery
│       • Platform-specific (Windows/Linux/macOS)
│       • RFCOMM binding (Linux)
│       • Device pairing
│
├── 🔌 FIRMWARE LAYER
│   └── arduino_sketch/matrix_display.ino
│       • ESP32/ESP8266 firmware
│       • Serial/Bluetooth receiver
│       • Binary protocol parser
│       • Pixel buffer management
│       • WS2812B LED driver
│       • Animation controller
│
└── 📖 DOCUMENTATION
    ├── README.md
    │   • Complete reference
    │   • All usage patterns
    │   • API documentation
    │   • Troubleshooting guide
    │
    ├── QUICKSTART.md
    │   • 5-minute setup
    │   • Quick reference
    │   • Common issues
    │
    └── ARCHITECTURE.md (this file)
        • System design
        • Data flows
        • Technical details
```

## Protocol Specification

### Binary Message Format

**Display Image Command (0x01)**
```
Byte 0:   0xFF          (Start marker)
Byte 1:   0x01          (Command: Display image)
Byte 2:   Width         (Unsigned 8-bit: 1-255)
Byte 3:   Height        (Unsigned 8-bit: 1-255)
Bytes 4+: Pixel data    (N bytes where N = Width × Height)

Each pixel byte represents brightness:
  0x00 = Black
  0x7F = Medium gray
  0xFF = White
```

**Example for 2×2 white image:**
```
Hex:  FF 01 02 02 FF FF FF FF
Dec:  255 1 2 2 255 255 255 255
```

**Animation Start Command (0x02)**
```
Byte 0:   0xFF              (Start marker)
Byte 1:   0x02              (Command: Animation start)
Byte 2:   Frame count       (1-255 frames)
Byte 3:   Delay H           (Upper byte of delay in ms)
Byte 4:   Delay L           (Lower byte of delay in ms)

Delay in milliseconds = (Byte 3 << 8) | Byte 4
Maximum delay = 65535ms (~65.5 seconds)
```

**Example for 10 frames, 100ms delay:**
```
Hex:  FF 02 0A 00 64
Dec:  255 2 10 0 100

10 = 0x0A
100 = 0x64 (since 100 < 256, it fits in lower byte)
```

## Communication Timing

### Serial (USB)
- Baud rate: 115200 (configurable to 9600 or 921600)
- Speed: ~14.4 KB/s practical throughput
- Latency: 1-10ms
- Range: Limited to USB cable length (~5m with powered hub)

### Bluetooth (ESP32 only)
- Standard: Bluetooth Classic (not BLE)
- Speed: ~250 kbps (~30 KB/s)
- Latency: 20-100ms
- Range: 10-100m depending on environment

### Frame Send Timing Example
For 32×8 matrix animation with 100ms frame delay:

```
Time  Event                      Data
─────────────────────────────────────────────────
0ms   Start animation command    [FF][02][10][00][64]
5ms   Frame 0 data arrives       [FF][01][20][08][...256 bytes...]
10ms  Frame 1 data arrives       [FF][01][20][08][...256 bytes...]
...
95ms  Frame 9 data arrives       [FF][01][20][08][...256 bytes...]
100ms Display frame 0
200ms Display frame 1
...
1000ms Display frame 9
1100ms Loop back to frame 0
```

## Processing Pipeline

### Image Processing (Sharp)

```javascript
Sharp workflow for static image:
1. Read image file
2. Resize to matrix dimensions (e.g., 32×8)
3. Convert to grayscale (remove color)
4. Extract raw pixel data
5. Apply threshold (> 127 → 255, else 0)
6. Pack into binary format
```

### GIF Processing (ImageIO)

```javascript
GIF workflow for animation:
1. Open GIF file
2. Extract all frames as images
3. For each frame:
   - Convert to RGB if indexed color
   - Resize to matrix dimensions
   - Convert to grayscale
   - Apply threshold
   - Store in frames array
4. Send animation command with frame count
5. Stream each frame with delay
```

## ESP32/ESP8266 Firmware Flow

```c
Setup phase:
├─ Initialize Serial at 115200 baud
├─ Initialize NeoPixel strip (GPIO5)
├─ Setup Bluetooth (if enabled)
└─ Show startup animation

Main loop:
├─ Check Serial.available()
├─ If packet received:
│  ├─ Wait for start byte (0xFF)
│  ├─ Read command byte
│  ├─ Switch on command:
│  │  ├─ 0x01 (Display): Parse dimensions and pixels
│  │  └─ 0x02 (Animation): Store frame count and delay
│  └─ Update pixel array
├─ Update LED display
└─ Repeat
```

## Dependency Tree

### Node.js Backend
```
express (HTTP server)
  ├─ body-parser (JSON parsing)
  └─ cors (CORS middleware)

socket.io (WebSocket)
  ├─ engine.io
  └─ socket.io-parser

serialport (Serial communication)
  ├─ @serialport/parser-readline
  └─ @serialport/bindings

sharp (Image processing)
  ├─ libvips (native binding)
  └─ imagemin (image optimization)

gif-frames (GIF extraction)
  ├─ jimp (image manipulation)
  └─ gif (GIF parsing)
```

### Python CLI
```
pyserial (Serial communication)
  └─ python-serial

PIL/Pillow (Image processing)
  ├─ libjpeg (JPEG support)
  ├─ libpng (PNG support)
  └─ libtiff (TIFF support)

imageio (Multimedia I/O)
  ├─ numpy
  ├─ PIL
  └─ ffmpeg (external)

pybluez (Bluetooth on Windows)
  └─ Windows Bluetooth API

wmi (Windows device info)
  └─ pywin32
```

## Performance Characteristics

### Memory Usage
- ESP32: ~4KB for display buffer (32×8×8bits)
- Spare RAM: ~240KB for animation frame queuing
- PC Backend: ~50-200MB (mostly image processing libraries)

### Processing Speed
- Image resize (32×8): ~50ms
- GIF frame extraction (10 frames): ~200-500ms
- Serial packet transmission: ~50ms per frame
- Bluetooth transmission: ~100-150ms per frame

### Display Update Rate
- USB Serial: 20 fps maximum (50ms per frame)
- Bluetooth: 10 fps maximum (100ms per frame)
- With compression: up to 30fps possible

## Error Handling

### Serial Communication
```
Scenario: Corrupted packet received
Response: Skip to next 0xFF marker, log error

Scenario: Timeout waiting for data
Response: Wait max 5 seconds, then fail with message

Scenario: Wrong matrix dimensions
Response: Send error response, don't update display
```

### Image Processing
```
Scenario: Unsupported image format
Response: Try PIL fallback, error if fails

Scenario: Image too large
Response: Resize handling (auto-fits to matrix)

Scenario: GIF has 0 frames
Response: Error message, don't proceed
```

### Connectivity
```
Scenario: Serial port disappears
Response: WebSocket notifies frontend, auto-reconnect option

Scenario: Bluetooth connection drops
Response: Retry connection with exponential backoff

Scenario: No response from ESP
Response: Timeout after 5s, suggest reset
```

## Scaling Considerations

### For Larger Displays (64×64)
- Packet size increases to 4096 bytes
- Processing time increases linearly
- Recommended: Reduce update rate or use Bluetooth for wireless

### For Multiple Matrices
- Daisy-chain WS2812B strips
- Update all pixels in single packet
- Or use multiple ESP boards with multi-master protocol

### For Real-time Video
- Pre-process video to GIF/frames
- Use frame skip for speed
- Or stream directly with custom protocol

## Security Notes

⚠️ **Important considerations:**
1. **No encryption** in standard protocol (local network only)
2. **No authentication** (assume trusted environment)
3. **Buffer overflow risk** in ESP if wrong dimensions
4. **DoS potential** from rapid packet flooding

For internet connectivity:
- Use TLS/SSL wrapper
- Add authentication token
- Rate-limit API endpoints
- Validate all inputs
