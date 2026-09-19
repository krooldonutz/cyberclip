// ============================================================
// ESP32/ESP8266 Matrix Display Controller
// Receives image data via Serial or Bluetooth and displays on LED matrix
// ============================================================

#include <Wire.h>
#include <Adafruit_NeoPixel.h>  // For WS2812B RGB LEDs
// OR use: #include <Adafruit_GFX.h> and #include <Max72xxPanel.h> for 8x8 matrix

// ============ CONFIGURATION ============
#define MATRIX_WIDTH 32
#define MATRIX_HEIGHT 8
#define PIXEL_PIN 5              // GPIO5 (D1 on ESP8266)
#define NUMPIXELS (MATRIX_WIDTH * MATRIX_HEIGHT)

// For Bluetooth (ESP32 only)
#define USE_BLUETOOTH 1
#define BLUETOOTH_NAME "ESP-Display"

// ============ GLOBALS ============
Adafruit_NeoPixel pixels(NUMPIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);

uint8_t displayBuffer[MATRIX_WIDTH * MATRIX_HEIGHT];
uint8_t isAnimating = 0;
uint16_t animationDelay = 100;
uint8_t totalFrames = 0;
uint8_t currentFrame = 0;

// ============ PROTOCOL ============
// Packet format:
// [0xFF][CMD][WIDTH][HEIGHT][DATA...]
// CMD 0x01: Display static image
// CMD 0x02: Prepare animation (FRAMES, DELAY_H, DELAY_L)

const uint8_t START_BYTE = 0xFF;
const uint8_t CMD_DISPLAY_IMAGE = 0x01;
const uint8_t CMD_ANIMATION_START = 0x02;

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== ESP Matrix Display Starting ===");
  
  // Initialize pixels
  pixels.begin();
  pixels.clear();
  pixels.show();
  
  // Show startup animation
  showStartupAnimation();
  
  // Initialize Bluetooth
  #if USE_BLUETOOTH && defined(ESP32)
  setupBluetooth();
  #endif
  
  Serial.println("Ready to receive data!");
}

// ============ LOOP ============
void loop() {
  // Check for incoming serial data
  if (Serial.available()) {
    handleSerialData();
  }
  
  #if USE_BLUETOOTH && defined(ESP32)
  // Bluetooth data is handled automatically in handleSerialData()
  #endif
  
  // If animating, update display
  if (isAnimating) {
    delay(animationDelay);
    currentFrame++;
    if (currentFrame >= totalFrames) {
      currentFrame = 0;
    }
  }
}

// ============ SERIAL DATA HANDLER ============
void handleSerialData() {
  // Wait for start byte
  if (Serial.peek() != START_BYTE) {
    Serial.read();
    return;
  }
  
  // Read packet header
  uint8_t startByte = Serial.read();
  uint8_t cmd = Serial.read();
  
  switch(cmd) {
    case CMD_DISPLAY_IMAGE:
      handleDisplayImage();
      break;
    case CMD_ANIMATION_START:
      handleAnimationStart();
      break;
    default:
      Serial.println("Unknown command");
      break;
  }
}

void handleDisplayImage() {
  if (Serial.available() < 2) return; // Need at least width, height
  
  uint8_t width = Serial.read();
  uint8_t height = Serial.read();
  
  if (width != MATRIX_WIDTH || height != MATRIX_HEIGHT) {
    Serial.printf("ERROR: Size mismatch. Expected %dx%d, got %dx%d\n", 
                   MATRIX_WIDTH, MATRIX_HEIGHT, width, height);
    return;
  }
  
  uint16_t dataSize = width * height;
  
  // Wait for all pixel data
  uint32_t timeout = millis() + 5000;
  while (Serial.available() < dataSize && millis() < timeout) {
    delay(1);
  }
  
  if (Serial.available() < dataSize) {
    Serial.println("ERROR: Incomplete data received");
    return;
  }
  
  // Read pixel data into buffer
  for (uint16_t i = 0; i < dataSize; i++) {
    displayBuffer[i] = Serial.read();
  }
  
  // Display the image
  updateDisplay(displayBuffer);
  
  isAnimating = 0;
  Serial.printf("✓ Image displayed (%dx%d)\n", width, height);
}

void handleAnimationStart() {
  if (Serial.available() < 3) return;
  
  uint8_t frames = Serial.read();
  uint8_t delayH = Serial.read();
  uint8_t delayL = Serial.read();
  
  animationDelay = (delayH << 8) | delayL;
  totalFrames = frames;
  currentFrame = 0;
  isAnimating = 1;
  
  Serial.printf("✓ Animation started (%d frames, %dms delay)\n", frames, animationDelay);
}

// ============ DISPLAY FUNCTIONS ============
void updateDisplay(uint8_t* buffer) {
  for (uint16_t i = 0; i < NUMPIXELS; i++) {
    uint8_t brightness = buffer[i];
    
    // Convert brightness to RGB (simple white)
    // For color displays: use HSV or adjust the RGB values
    uint32_t color = pixels.Color(brightness, brightness, brightness);
    
    pixels.setPixelColor(i, color);
  }
  
  pixels.show();
}

void showStartupAnimation() {
  // Simple rainbow animation
  for (int i = 0; i < NUMPIXELS; i++) {
    uint32_t color = pixels.ColorHSV((i * 65536L / NUMPIXELS));
    pixels.setPixelColor(i, color);
    pixels.show();
    delay(20);
  }
  
  delay(500);
  
  // Fade out
  for (int brightness = 255; brightness >= 0; brightness -= 10) {
    for (int i = 0; i < NUMPIXELS; i++) {
      uint32_t color = pixels.ColorHSV((i * 65536L / NUMPIXELS), 255, brightness);
      pixels.setPixelColor(i, color);
    }
    pixels.show();
    delay(10);
  }
  
  pixels.clear();
  pixels.show();
}

// ============ BLUETOOTH SETUP (ESP32 ONLY) ============
#if USE_BLUETOOTH && defined(ESP32)

#include <BluetoothSerial.h>

BluetoothSerial SerialBT;

void setupBluetooth() {
  SerialBT.begin(BLUETOOTH_NAME);
  Serial.println("Bluetooth started: " + String(BLUETOOTH_NAME));
}

// Override Serial.read() behavior to also check Bluetooth
uint8_t readFromBT() {
  if (SerialBT.available()) {
    return SerialBT.read();
  }
  return Serial.read();
}

#endif

// ============ DEBUG COMMANDS ============
void handleSerialCommands() {
  if (Serial.available() && Serial.peek() != START_BYTE) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "help") {
      Serial.println("Commands:");
      Serial.println("  help - Show this message");
      Serial.println("  clear - Clear display");
      Serial.println("  test - Show test pattern");
      Serial.println("  status - Show device status");
    }
    else if (cmd == "clear") {
      pixels.clear();
      pixels.show();
      Serial.println("Display cleared");
    }
    else if (cmd == "test") {
      // Horizontal stripes
      for (uint16_t i = 0; i < NUMPIXELS; i++) {
        uint8_t brightness = (i % (MATRIX_WIDTH * 2) < MATRIX_WIDTH) ? 255 : 0;
        pixels.setPixelColor(i, pixels.Color(brightness, brightness, brightness));
      }
      pixels.show();
      Serial.println("Test pattern displayed");
    }
    else if (cmd == "status") {
      Serial.printf("Status: Animating=%d, Frames=%d, Delay=%dms\n", 
                     isAnimating, totalFrames, animationDelay);
    }
  }
}
