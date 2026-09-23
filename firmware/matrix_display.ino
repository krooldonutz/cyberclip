#include <Arduino.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <LovyanGFX.hpp>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>

#include "protocol.h"
#include "src/wifi_manager.h"
#include "src/ws_server.h"

namespace board {
constexpr int kMosi = 23;
constexpr int kSclk = 18;
constexpr int kCs = 15;
constexpr int kDc = 2;
constexpr int kReset = 4;
constexpr int kBacklight = 32;
constexpr uint16_t kWidth = 170;
constexpr uint16_t kHeight = 320;

// This 1.9" board exposes a centered 170x320 window in ST7789 RAM.
constexpr uint16_t kControllerWidth = 240;
constexpr uint16_t kControllerHeight = 320;
constexpr uint16_t kOffsetX = 35;
constexpr uint16_t kOffsetY = 0;
constexpr bool kInvert = true;
constexpr bool kRgbOrder = false;
constexpr uint32_t kSpiFrequency = 40000000;
constexpr uint8_t kBacklightPwmChannel = 7;
}  // namespace board

class MatrixDisplay : public lgfx::LGFX_Device {
 public:
  MatrixDisplay() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = board::kSpiFrequency;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = board::kSclk;
      cfg.pin_mosi = board::kMosi;
      cfg.pin_miso = -1;
      cfg.pin_dc = board::kDc;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = board::kCs;
      cfg.pin_rst = board::kReset;
      cfg.pin_busy = -1;
      cfg.memory_width = board::kControllerWidth;
      cfg.memory_height = board::kControllerHeight;
      cfg.panel_width = board::kWidth;
      cfg.panel_height = board::kHeight;
      cfg.offset_x = board::kOffsetX;
      cfg.offset_y = board::kOffsetY;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = board::kInvert;
      cfg.rgb_order = board::kRgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = board::kBacklight;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = board::kBacklightPwmChannel;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    setPanel(&panel_);
  }

 private:
  lgfx::Bus_SPI bus_;
  lgfx::Panel_ST7789 panel_;
  lgfx::Light_PWM light_;
};

using namespace cyberclip;

namespace {
constexpr uint32_t kSerialBaud = 921600;
constexpr uint32_t kMaxFrameSize = 128 * 1024;
constexpr uint32_t kConservativeStoredBytes = 8 * 1024 * 1024;
constexpr uint32_t kParserTimeoutMs = 1000;
constexpr uint16_t kMinimumFrameDelayMs = 10;
constexpr gpio_num_t kSleepButton = GPIO_NUM_0;
constexpr uint32_t kSleepButtonDebounceMs = 30;
constexpr uint32_t kHotspotButtonHoldMs = 1500;
constexpr uint8_t kFirmwareMajor = 2;
constexpr uint8_t kFirmwareMinor = 0;
constexpr uint8_t kFirmwarePatch = 12;
constexpr char kDeviceName[] = "CyberClip Ideaspark ESP32 ST7789";
constexpr char kMetadataPath[] = "/playlist.meta";
constexpr char kMetadataTempPath[] = "/playlist.tmp";
constexpr char kProgressLabel[] = "Media is being loaded";
constexpr uint16_t kProgressBackgroundColor = 0x0000;
constexpr uint16_t kProgressBorderColor = 0xffff;
constexpr uint16_t kProgressFillColor = 0x05ff;
constexpr int16_t kProgressMargin = 16;
constexpr int16_t kProgressHeight = 18;
constexpr int16_t kProgressBorder = 2;

MatrixDisplay display;
JPEGDEC jpeg;

struct Transfer {
  uint8_t *data = nullptr;
  uint32_t total = 0;
  uint32_t received = 0;
  uint16_t id = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t frameIndex = 0;
  uint16_t delayMs = 0;
  uint8_t rotation = 0;
  bool persistent = false;
  bool active = false;
} transfer;

struct Playlist {
  uint16_t delays[255]{};
  uint16_t frameCount = 0;
  uint16_t storedCount = 0;
  uint8_t mediaType = 0;
  uint8_t rotation = 0;
  uint8_t generation = 0;
  bool loop = false;
  bool valid = false;
} activePlaylist, stagingPlaylist;

struct Playback {
  uint16_t nextFrame = 0;
  uint32_t deadline = 0;
  bool running = false;
} playback;

struct UploadProgress {
  int16_t x = 0;
  int16_t y = 0;
  int16_t innerWidth = 0;
  uint16_t filledPixels = UINT16_MAX;
  bool visible = false;
} uploadProgress;

uint8_t backlight = 255;
bool renderToDisplay = false;
bool filesystemMounted = false;
bool sleepButtonArmed = false;
bool sleepButtonPressed = false;
bool sleepButtonLongHandled = false;
uint32_t sleepButtonPressedAt = 0;

// Selects where writeFrame() sends its next reply. Set immediately before
// feeding bytes into serialParser/wsParser in loop(), so it is always
// correct for the synchronous sendAck/sendNack/sendHello/sendStatus calls
// that happen while handling that byte.
enum class ReplyTarget { kSerial, kWebSocket };
ReplyTarget currentReplyTarget = ReplyTarget::kSerial;

bool startStoredPlayback();

void showUploadProgress(uint8_t rotation) {
  display.setRotation(rotation);
  display.fillScreen(kProgressBackgroundColor);
  const int16_t width = display.width() - kProgressMargin * 2;
  uploadProgress.x = kProgressMargin;
  uploadProgress.y = (display.height() - kProgressHeight) / 2;
  uploadProgress.innerWidth = width - kProgressBorder * 2;
  uploadProgress.filledPixels = UINT16_MAX;
  uploadProgress.visible = true;
  display.setTextSize(1);
  display.setTextColor(kProgressBorderColor, kProgressBackgroundColor);
  const int16_t labelX = (display.width() - display.textWidth(kProgressLabel)) / 2;
  display.setCursor(labelX > 0 ? labelX : 0,
                    uploadProgress.y - display.fontHeight() - 10);
  display.print(kProgressLabel);
  display.drawRect(uploadProgress.x, uploadProgress.y, width, kProgressHeight,
                   kProgressBorderColor);
  display.drawRect(uploadProgress.x + 1, uploadProgress.y + 1, width - 2,
                   kProgressHeight - 2, kProgressBorderColor);
}

void updateUploadProgress(uint16_t completedFrames, uint32_t receivedBytes,
                          uint32_t totalBytes) {
  if (!uploadProgress.visible || !stagingPlaylist.valid) return;
  const uint16_t filled = uploadProgressPixels(
      uploadProgress.innerWidth, stagingPlaylist.frameCount, completedFrames,
      receivedBytes, totalBytes);
  if (filled == uploadProgress.filledPixels) return;
  const int16_t innerX = uploadProgress.x + kProgressBorder;
  const int16_t innerY = uploadProgress.y + kProgressBorder;
  const int16_t innerHeight = kProgressHeight - kProgressBorder * 2;
  if (filled > 0) {
    display.fillRect(innerX, innerY, filled, innerHeight, kProgressFillColor);
  }
  if (filled < uploadProgress.innerWidth) {
    display.fillRect(innerX + filled, innerY,
                     uploadProgress.innerWidth - filled, innerHeight,
                     kProgressBackgroundColor);
  }
  uploadProgress.filledPixels = filled;
}

void restoreActiveOrClear(uint8_t fallbackRotation) {
  uploadProgress = UploadProgress{};
  if (activePlaylist.valid && startStoredPlayback()) return;
  display.setRotation(fallbackRotation);
  display.fillScreen(TFT_BLACK);
}

void enterDeepSleep() {
  display.setBrightness(0);
  display.sleep();
  Serial.flush();
  gpio_hold_dis(static_cast<gpio_num_t>(board::kBacklight));
  pinMode(board::kBacklight, OUTPUT);
  digitalWrite(board::kBacklight, LOW);
  gpio_hold_en(static_cast<gpio_num_t>(board::kBacklight));

  esp_sleep_enable_ext0_wakeup(kSleepButton, LOW);
  esp_deep_sleep_start();
}

void showHotspotButtonFeedback(uint8_t previousMode) {
  const HotspotStatus status = wifiManager.hotspotStatus();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_center);
  display.setTextSize(2);
  if (status.mode != previousMode) {
    display.drawString(status.mode == HOTSPOT_OFF ? "Hotspot off" : "Hotspot on",
                       display.width() / 2, display.height() / 2);
  } else {
    display.drawString("Set AP password", display.width() / 2,
                       display.height() / 2);
  }
}

void pollSleepButton() {
  const bool pressed = digitalRead(kSleepButton) == LOW;
  if (!sleepButtonArmed) {
    if (!pressed) sleepButtonArmed = true;
    return;
  }

  if (pressed && !sleepButtonPressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(kSleepButton) != LOW) return;
    sleepButtonPressed = true;
    sleepButtonLongHandled = false;
    sleepButtonPressedAt = millis();
    return;
  }

  if (pressed && sleepButtonPressed && !sleepButtonLongHandled &&
      millis() - sleepButtonPressedAt >= kHotspotButtonHoldMs) {
    const uint8_t previousMode = wifiManager.hotspotStatus().mode;
    wifiManager.toggleHotspotMode();
    showHotspotButtonFeedback(previousMode);
    sleepButtonLongHandled = true;
    return;
  }

  if (!pressed && sleepButtonPressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(kSleepButton) == LOW) return;
    const bool wasLongPress = sleepButtonLongHandled;
    sleepButtonPressed = false;
    sleepButtonLongHandled = false;
    if (!wasLongPress) enterDeepSleep();
  }
}

void releaseTransfer() {
  free(transfer.data);
  transfer = Transfer{};
}

void framePath(char *path, size_t length, uint8_t generation,
               uint16_t frameIndex) {
  snprintf(path, length, "/%c%03u.jpg", generation ? 'b' : 'a', frameIndex);
}

bool removeGeneration(uint8_t generation) {
  if (!filesystemMounted) return false;
  bool removed = true;
  char path[12];
  for (uint16_t i = 0; i < 255; ++i) {
    framePath(path, sizeof(path), generation, i);
    if (LittleFS.exists(path) && !LittleFS.remove(path)) removed = false;
  }
  return removed;
}

bool cancelStaging() {
  if (!stagingPlaylist.valid) return true;
  const uint8_t rotation = stagingPlaylist.rotation;
  if (transfer.active && transfer.persistent) releaseTransfer();
  const bool removed = removeGeneration(stagingPlaylist.generation);
  const bool tempRemoved =
      !LittleFS.exists(kMetadataTempPath) ||
      LittleFS.remove(kMetadataTempPath);
  stagingPlaylist = Playlist{};
  restoreActiveOrClear(rotation);
  return removed && tempRemoved;
}

void writeFrame(uint8_t command, uint16_t sequence, const uint8_t *payload,
                uint32_t payloadLength) {
  uint8_t header[10] = {
      kMagic0, kMagic1, kProtocolVersion, command,
      static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8),
  };
  writeLe32(header + 6, payloadLength);

  uint16_t crc = 0xFFFF;
  for (size_t i = 2; i < sizeof(header); ++i) {
    crc = crc16CcittFalseUpdate(crc, header[i]);
  }
  for (uint32_t i = 0; i < payloadLength; ++i) {
    crc = crc16CcittFalseUpdate(crc, payload[i]);
  }
  const uint8_t trailer[2] = {
      static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8)};

  if (currentReplyTarget == ReplyTarget::kSerial) {
    Serial.write(header, sizeof(header));
    if (payloadLength) Serial.write(payload, payloadLength);
    Serial.write(trailer, sizeof(trailer));
    return;
  }

  static uint8_t wsFrame[sizeof(header) + kMaxWirePayload + sizeof(trailer)];
  memcpy(wsFrame, header, sizeof(header));
  if (payloadLength) memcpy(wsFrame + sizeof(header), payload, payloadLength);
  memcpy(wsFrame + sizeof(header) + payloadLength, trailer, sizeof(trailer));
  wsSendFrame(wsFrame, sizeof(header) + payloadLength + sizeof(trailer));
}

void sendAck(uint16_t sequence, uint8_t requestCommand) {
  writeFrame(ACK, sequence, &requestCommand, 1);
}

void sendNack(uint16_t sequence, uint8_t requestCommand, ErrorCode error) {
  const uint8_t payload[] = {requestCommand, static_cast<uint8_t>(error)};
  writeFrame(NACK, sequence, payload, sizeof(payload));
}

bool dimensionsMatch(uint16_t width, uint16_t height, uint8_t rotation) {
  return rotation < 4 &&
         ((rotation & 1) ? (width == board::kHeight && height == board::kWidth)
                         : (width == board::kWidth && height == board::kHeight));
}

int jpegDraw(JPEGDRAW *draw) {
  if (renderToDisplay) {
    display.pushImage(draw->x, draw->y, draw->iWidth, draw->iHeight,
                      draw->pPixels);
  }
  return 1;
}

bool decodeJpegBuffer(uint8_t *data, uint32_t size, uint16_t width,
                      uint16_t height, uint8_t rotation, bool render) {
  if (!jpeg.openRAM(data, static_cast<int>(size), jpegDraw)) return false;
  const bool dimensionsOk =
      jpeg.getWidth() == width && jpeg.getHeight() == height;
  bool decoded = false;
  if (dimensionsOk) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    renderToDisplay = render;
    if (render) {
      display.setRotation(rotation);
      display.startWrite();
    }
    decoded = jpeg.decode(0, 0, 0) != 0;
    if (render) display.endWrite();
    renderToDisplay = false;
  }
  jpeg.close();
  return dimensionsOk && decoded;
}

bool decodeTransfer(bool render) {
  return decodeJpegBuffer(transfer.data, transfer.total, transfer.width,
                          transfer.height, transfer.rotation, render);
}

bool readFileToBuffer(const char *path, uint8_t **data, uint32_t *size) {
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  const size_t fileSize = file.size();
  if (fileSize == 0 || fileSize > kMaxFrameSize) {
    file.close();
    return false;
  }
  auto *buffer = static_cast<uint8_t *>(
      heap_caps_malloc(fileSize, MALLOC_CAP_8BIT));
  if (!buffer) {
    file.close();
    return false;
  }
  const size_t bytesRead = file.read(buffer, fileSize);
  file.close();
  if (bytesRead != fileSize) {
    free(buffer);
    return false;
  }
  *data = buffer;
  *size = static_cast<uint32_t>(fileSize);
  return true;
}

bool renderPlaylistFrame(const Playlist &playlist, uint16_t frameIndex) {
  if (!filesystemMounted || !playlist.valid ||
      frameIndex >= playlist.frameCount) {
    return false;
  }
  char path[12];
  framePath(path, sizeof(path), playlist.generation, frameIndex);
  uint8_t *data = nullptr;
  uint32_t size = 0;
  if (!readFileToBuffer(path, &data, &size)) return false;
  const uint16_t width =
      (playlist.rotation & 1) ? board::kHeight : board::kWidth;
  const uint16_t height =
      (playlist.rotation & 1) ? board::kWidth : board::kHeight;
  const bool rendered =
      decodeJpegBuffer(data, size, width, height, playlist.rotation, true);
  free(data);
  return rendered;
}

bool playlistFilesExist(const Playlist &playlist) {
  char path[12];
  for (uint16_t i = 0; i < playlist.frameCount; ++i) {
    framePath(path, sizeof(path), playlist.generation, i);
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.size() == 0 || file.size() > kMaxFrameSize) {
      if (file) file.close();
      return false;
    }
    file.close();
  }
  return true;
}

bool loadActiveMetadata() {
  activePlaylist = Playlist{};
  if (!filesystemMounted) return false;
  File file = LittleFS.open(kMetadataPath, FILE_READ);
  if (!file) return false;
  const size_t size = file.size();
  if (size < playlistMetadataSize(1) || size > playlistMetadataSize(255)) {
    file.close();
    return false;
  }
  uint8_t metadata[playlistMetadataSize(255)];
  if (file.read(metadata, size) != size) {
    file.close();
    return false;
  }
  file.close();
  if (!validatePlaylistMetadata(metadata, size)) return false;

  Playlist loaded;
  loaded.generation = metadata[5];
  loaded.mediaType = metadata[6];
  loaded.frameCount = metadata[7];
  loaded.storedCount = loaded.frameCount;
  loaded.rotation = metadata[8];
  loaded.loop = metadata[9] != 0;
  loaded.valid = true;
  for (uint16_t i = 0; i < loaded.frameCount; ++i) {
    loaded.delays[i] =
        readLe16(metadata + kPlaylistMetadataHeaderSize + i * 2);
    if (loaded.delays[i] < kMinimumFrameDelayMs) return false;
  }
  if (!playlistFilesExist(loaded)) return false;
  activePlaylist = loaded;
  return true;
}

bool writeStagingMetadata() {
  const size_t size =
      playlistMetadataSize(static_cast<uint8_t>(stagingPlaylist.frameCount));
  uint8_t metadata[playlistMetadataSize(255)]{};
  metadata[0] = 'C';
  metadata[1] = 'C';
  metadata[2] = 'P';
  metadata[3] = 'L';
  metadata[4] = kPlaylistMetadataVersion;
  metadata[5] = stagingPlaylist.generation;
  metadata[6] = stagingPlaylist.mediaType;
  metadata[7] = static_cast<uint8_t>(stagingPlaylist.frameCount);
  metadata[8] = stagingPlaylist.rotation;
  metadata[9] = stagingPlaylist.loop ? 1 : 0;
  for (uint16_t i = 0; i < stagingPlaylist.frameCount; ++i) {
    writeLe16(metadata + kPlaylistMetadataHeaderSize + i * 2,
              stagingPlaylist.delays[i]);
  }
  writeLe16(metadata + size - kPlaylistMetadataCrcSize,
            crc16CcittFalse(metadata, size - kPlaylistMetadataCrcSize));

  LittleFS.remove(kMetadataTempPath);
  File file = LittleFS.open(kMetadataTempPath, FILE_WRITE);
  if (!file) return false;
  const bool written = file.write(metadata, size) == size;
  file.flush();
  file.close();
  if (!written) {
    LittleFS.remove(kMetadataTempPath);
    return false;
  }

  File verify = LittleFS.open(kMetadataTempPath, FILE_READ);
  uint8_t check[playlistMetadataSize(255)];
  const bool verified =
      verify && verify.size() == size && verify.read(check, size) == size &&
      validatePlaylistMetadata(check, size) &&
      memcmp(metadata, check, size) == 0;
  if (verify) verify.close();
  if (!verified) {
    LittleFS.remove(kMetadataTempPath);
    return false;
  }
  return true;
}

void schedulePlaybackAfterFirstFrame() {
  playback = Playback{};
  if (activePlaylist.mediaType == MEDIA_GIF &&
      activePlaylist.frameCount > 1) {
    playback.running = true;
    playback.nextFrame = 1;
    playback.deadline = millis() + activePlaylist.delays[0];
  }
}

bool startStoredPlayback() {
  playback = Playback{};
  if (!activePlaylist.valid || !renderPlaylistFrame(activePlaylist, 0)) {
    return false;
  }
  schedulePlaybackAfterFirstFrame();
  return true;
}

void advanceStoredPlayback() {
  if (!playback.running ||
      static_cast<int32_t>(millis() - playback.deadline) < 0) {
    return;
  }

  uint32_t now = millis();
  uint16_t frame = playback.nextFrame;
  while (true) {
    const bool lastFrame = frame + 1 >= activePlaylist.frameCount;
    const uint32_t frameEnd = playback.deadline + activePlaylist.delays[frame];
    if (static_cast<int32_t>(now - frameEnd) < 0 ||
        (lastFrame && !activePlaylist.loop)) {
      break;
    }
    playback.deadline = frameEnd;
    frame = lastFrame ? 0 : frame + 1;
    playback.nextFrame = frame;
  }

  if (!renderPlaylistFrame(activePlaylist, frame)) {
    playback.running = false;
    return;
  }
  if (frame + 1 >= activePlaylist.frameCount) {
    if (!activePlaylist.loop) {
      playback.running = false;
      return;
    }
    playback.nextFrame = 0;
  } else {
    playback.nextFrame = frame + 1;
  }
  playback.deadline += activePlaylist.delays[frame];
}

void sendHello(uint16_t sequence) {
  constexpr size_t kFixedLength = kHelloResponseFixedSize;
  uint8_t payload[kFixedLength + sizeof(kDeviceName) - 1];
  writeLe16(payload, board::kWidth);
  writeLe16(payload + 2, board::kHeight);
  writeLe16(payload + 4, kMaxChunkData);
  writeLe32(payload + 6, kMaxFrameSize);
  payload[10] = 0x0F;
  payload[11] = 0x01;
  payload[12] = kFirmwareMajor;
  payload[13] = kFirmwareMinor;
  payload[14] = kFirmwarePatch;
  payload[15] = 1;
  const uint32_t storedBytes =
      filesystemMounted ? static_cast<uint32_t>(LittleFS.totalBytes())
                        : kConservativeStoredBytes;
  writeLe32(payload + 16, storedBytes);
  memcpy(payload + kFixedLength, kDeviceName, sizeof(kDeviceName) - 1);
  writeFrame(HELLO_RESPONSE, sequence, payload, sizeof(payload));
}

void sendStatus(uint16_t sequence) {
  // STATUS_RESPONSE remains: transferActive u8, backlight u8, transferId u16,
  // received u32, expected u32, freeHeap u32 (little-endian).
  uint8_t payload[16] = {static_cast<uint8_t>(transfer.active), backlight};
  writeLe16(payload + 2, transfer.active ? transfer.id : 0);
  writeLe32(payload + 4, transfer.received);
  writeLe32(payload + 8, transfer.total);
  writeLe32(payload + 12, ESP.getFreeHeap());
  writeFrame(STATUS_RESPONSE, sequence, payload, sizeof(payload));
}

void sendWifiStatus(uint16_t sequence, bool tokenIncluded,
                    const uint8_t *token) {
  const WifiStatus wifiStatus = wifiManager.status();
  const size_t hostnameLength = strlen(wifiStatus.hostname);
  uint8_t payload[1 + 4 + 1 + sizeof(wifiStatus.hostname) - 1 + 1 +
                  kWifiTokenSize];
  size_t offset = 0;
  payload[offset++] = wifiStatus.state;
  memcpy(payload + offset, wifiStatus.ip, sizeof(wifiStatus.ip));
  offset += sizeof(wifiStatus.ip);
  payload[offset++] = static_cast<uint8_t>(hostnameLength);
  memcpy(payload + offset, wifiStatus.hostname, hostnameLength);
  offset += hostnameLength;
  payload[offset++] = tokenIncluded ? 1 : 0;
  if (tokenIncluded && token) {
    memcpy(payload + offset, token, kWifiTokenSize);
    offset += kWifiTokenSize;
  }
  writeFrame(WIFI_STATUS_RESPONSE, sequence, payload, offset);
}

void sendHotspotStatus(uint16_t sequence) {
  const HotspotStatus hotspotStatus = wifiManager.hotspotStatus();
  const size_t ssidLength = strlen(hotspotStatus.ssid);
  uint8_t payload[8 + kWifiMaxSsidLength];
  payload[0] = hotspotStatus.mode;
  payload[1] = hotspotStatus.running ? 1 : 0;
  payload[2] = hotspotStatus.passwordConfigured ? 1 : 0;
  memcpy(payload + 3, hotspotStatus.ip, sizeof(hotspotStatus.ip));
  payload[7] = static_cast<uint8_t>(ssidLength);
  memcpy(payload + 8, hotspotStatus.ssid, ssidLength);
  writeFrame(HOTSPOT_STATUS_RESPONSE, sequence, payload, 8 + ssidLength);
}

bool persistTransferFrame() {
  char path[12];
  framePath(path, sizeof(path), stagingPlaylist.generation,
            transfer.frameIndex);
  LittleFS.remove(path);
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) return false;
  const bool written = file.write(transfer.data, transfer.total) == transfer.total;
  file.flush();
  file.close();
  if (!written) {
    LittleFS.remove(path);
    return false;
  }
  File verify = LittleFS.open(path, FILE_READ);
  const bool validSize = verify && verify.size() == transfer.total;
  if (verify) verify.close();
  if (!validSize) LittleFS.remove(path);
  return validSize;
}

bool clearStoredMedia() {
  playback = Playback{};
  releaseTransfer();
  stagingPlaylist = Playlist{};
  if (!filesystemMounted) return false;
  const bool metadataRemoved =
      !LittleFS.exists(kMetadataPath) || LittleFS.remove(kMetadataPath);
  const bool tempRemoved =
      !LittleFS.exists(kMetadataTempPath) ||
      LittleFS.remove(kMetadataTempPath);
  const bool generationARemoved = removeGeneration(0);
  const bool generationBRemoved = removeGeneration(1);
  const bool generationsRemoved =
      generationARemoved && generationBRemoved;
  if (metadataRemoved && tempRemoved && generationsRemoved) {
    activePlaylist = Playlist{};
    return true;
  }
  if (loadActiveMetadata()) startStoredPlayback();
  return false;
}

void handleCommand(uint8_t command, uint16_t sequence, const uint8_t *payload,
                   uint32_t length) {
  switch (command) {
    case HELLO:
      if (length != 0) sendNack(sequence, command, INVALID_PAYLOAD);
      else sendHello(sequence);
      return;

    case BEGIN_FRAME: {
      const bool persistent = stagingPlaylist.valid;
      if (length != (persistent ? 16U : 12U)) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (persistent && transfer.active) {
        sendNack(sequence, command, cyberclip::BUSY);
        return;
      }
      const uint16_t id = readLe16(payload);
      const uint16_t width = readLe16(payload + 2);
      const uint16_t height = readLe16(payload + 4);
      const uint8_t codec = payload[6];
      const uint8_t rotation = payload[7];
      const uint32_t total = readLe32(payload + 8);
      if (codec != 1 || !dimensionsMatch(width, height, rotation) ||
          total == 0 || total > kMaxFrameSize) {
        sendNack(sequence, command, OUT_OF_RANGE);
        return;
      }
      uint16_t frameIndex = 0;
      uint16_t delayMs = 0;
      if (persistent) {
        frameIndex = readLe16(payload + 12);
        delayMs = readLe16(payload + 14);
        if (frameIndex != stagingPlaylist.storedCount ||
            frameIndex >= stagingPlaylist.frameCount ||
            rotation != stagingPlaylist.rotation) {
          sendNack(sequence, command, SEQUENCE_ERROR);
          return;
        }
      }
      releaseTransfer();
      auto *buffer = static_cast<uint8_t *>(
          heap_caps_malloc(total, MALLOC_CAP_8BIT));
      if (!buffer) {
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      transfer.data = buffer;
      transfer.total = total;
      transfer.id = id;
      transfer.width = width;
      transfer.height = height;
      transfer.frameIndex = frameIndex;
      transfer.delayMs = delayMs;
      transfer.rotation = rotation;
      transfer.persistent = persistent;
      transfer.active = true;
      if (persistent) {
        updateUploadProgress(stagingPlaylist.storedCount, 0, transfer.total);
      }
      sendAck(sequence, command);
      return;
    }

    case FRAME_CHUNK: {
      if (length < 7 || length - 6 > kMaxChunkData) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (!transfer.active) {
        sendNack(sequence, command, NO_ACTIVE_TRANSFER);
        return;
      }
      const uint16_t id = readLe16(payload);
      const uint32_t offset = readLe32(payload + 2);
      const uint32_t dataLength = length - 6;
      if (id != transfer.id || offset != transfer.received) {
        sendNack(sequence, command, SEQUENCE_ERROR);
        return;
      }
      if (dataLength > transfer.total - transfer.received) {
        sendNack(sequence, command, OUT_OF_RANGE);
        return;
      }
      memcpy(transfer.data + transfer.received, payload + 6, dataLength);
      transfer.received += dataLength;
      if (transfer.persistent) {
        updateUploadProgress(stagingPlaylist.storedCount, transfer.received,
                             transfer.total);
      }
      sendAck(sequence, command);
      return;
    }

    case COMMIT_FRAME: {
      if (length != 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (!transfer.active) {
        sendNack(sequence, command, NO_ACTIVE_TRANSFER);
        return;
      }
      if (readLe16(payload) != transfer.id ||
          transfer.received != transfer.total) {
        sendNack(sequence, command, SEQUENCE_ERROR);
        return;
      }
      if (!decodeTransfer(false)) {
        sendNack(sequence, command, DECODE_FAILED);
        releaseTransfer();
        return;
      }
      if (transfer.persistent && !persistTransferFrame()) {
        sendNack(sequence, command, NO_MEMORY);
        releaseTransfer();
        return;
      }
      if (transfer.persistent) {
        stagingPlaylist.delays[transfer.frameIndex] =
            max(transfer.delayMs, kMinimumFrameDelayMs);
        ++stagingPlaylist.storedCount;
        updateUploadProgress(stagingPlaylist.storedCount, 0, 0);
      } else if (!decodeTransfer(true)) {
        sendNack(sequence, command, DECODE_FAILED);
        releaseTransfer();
        return;
      }
      sendAck(sequence, command);
      releaseTransfer();
      return;
    }

    case CANCEL_TRANSFER: {
      if (length != 0 && length != 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (!transfer.active && !stagingPlaylist.valid) {
        sendNack(sequence, command, NO_ACTIVE_TRANSFER);
        return;
      }
      if (transfer.active && length == 2 &&
          readLe16(payload) != transfer.id) {
        sendNack(sequence, command, SEQUENCE_ERROR);
        return;
      }
      if (transfer.active) releaseTransfer();
      const bool cleaned = !stagingPlaylist.valid || cancelStaging();
      if (cleaned) sendAck(sequence, command);
      else sendNack(sequence, command, NO_MEMORY);
      return;
    }

    case SET_BACKLIGHT:
      if (length != 1) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        backlight = payload[0];
        display.setBrightness(backlight);
        sendAck(sequence, command);
      }
      return;

    case CLEAR_DISPLAY:
      if (length != 0 && length != 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        display.fillScreen(length ? readLe16(payload) : 0);
        sendAck(sequence, command);
      }
      return;

    case GET_STATUS:
      if (length != 0) sendNack(sequence, command, INVALID_PAYLOAD);
      else sendStatus(sequence);
      return;

    case BEGIN_PLAYLIST: {
      if (length != 5) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (!filesystemMounted) {
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      if (transfer.active || stagingPlaylist.valid) {
        sendNack(sequence, command, cyberclip::BUSY);
        return;
      }
      const uint8_t mediaType = payload[0];
      const uint16_t frameCount = readLe16(payload + 1);
      const bool loop = payload[3] != 0;
      const uint8_t rotation = payload[4];
      if ((mediaType != MEDIA_IMAGE && mediaType != MEDIA_GIF) ||
          frameCount == 0 || frameCount > 255 || payload[3] > 1 ||
          rotation > 3 || (mediaType == MEDIA_IMAGE && frameCount != 1)) {
        sendNack(sequence, command, OUT_OF_RANGE);
        return;
      }
      const uint8_t generation =
          activePlaylist.valid ? activePlaylist.generation ^ 1 : 0;
      if (!removeGeneration(generation) ||
          (LittleFS.exists(kMetadataTempPath) &&
           !LittleFS.remove(kMetadataTempPath))) {
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      stagingPlaylist = Playlist{};
      stagingPlaylist.mediaType = mediaType;
      stagingPlaylist.frameCount = frameCount;
      stagingPlaylist.rotation = rotation;
      stagingPlaylist.generation = generation;
      stagingPlaylist.loop = loop;
      stagingPlaylist.valid = true;
      playback.running = false;
      showUploadProgress(rotation);
      updateUploadProgress(0, 0, 0);
      sendAck(sequence, command);
      return;
    }

    case END_PLAYLIST: {
      if (length != 0) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (!stagingPlaylist.valid) {
        sendNack(sequence, command, NO_ACTIVE_TRANSFER);
        return;
      }
      if (transfer.active) {
        sendNack(sequence, command, cyberclip::BUSY);
        return;
      }
      if (stagingPlaylist.storedCount != stagingPlaylist.frameCount) {
        sendNack(sequence, command, SEQUENCE_ERROR);
        return;
      }
      if (!writeStagingMetadata()) {
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      const Playlist previous = activePlaylist;
      if (!renderPlaylistFrame(stagingPlaylist, 0)) {
        LittleFS.remove(kMetadataTempPath);
        sendNack(sequence, command, DECODE_FAILED);
        return;
      }
      if (!LittleFS.rename(kMetadataTempPath, kMetadataPath)) {
        activePlaylist = previous;
        restoreActiveOrClear(stagingPlaylist.rotation);
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      activePlaylist = stagingPlaylist;
      activePlaylist.valid = true;
      stagingPlaylist = Playlist{};
      uploadProgress = UploadProgress{};
      schedulePlaybackAfterFirstFrame();
      if (previous.valid &&
          previous.generation != activePlaylist.generation) {
        removeGeneration(previous.generation);
      }
      sendAck(sequence, command);
      return;
    }

    case PLAY_STORED:
      if (length != 0) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else if (!loadActiveMetadata() || !startStoredPlayback()) {
        sendNack(sequence, command, DECODE_FAILED);
      } else {
        sendAck(sequence, command);
      }
      return;

    case CLEAR_STORED:
      if (length != 0) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        if (clearStoredMedia()) sendAck(sequence, command);
        else sendNack(sequence, command, NO_MEMORY);
      }
      return;

    case SET_WIFI_CREDENTIALS: {
      if (currentReplyTarget != ReplyTarget::kSerial || length < 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      const uint8_t ssidLength = payload[0];
      if (ssidLength == 0 || ssidLength > kWifiMaxSsidLength ||
          length < static_cast<uint32_t>(1 + ssidLength + 1)) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      const uint8_t passwordLength = payload[1 + ssidLength];
      if (passwordLength > kWifiMaxPasswordLength ||
          length != static_cast<uint32_t>(1 + ssidLength + 1 + passwordLength)) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      char ssid[kWifiMaxSsidLength + 1] = {};
      char password[kWifiMaxPasswordLength + 1] = {};
      memcpy(ssid, payload + 1, ssidLength);
      memcpy(password, payload + 1 + ssidLength + 1, passwordLength);
      uint8_t token[kWifiTokenSize];
      bool tokenIncluded = false;
      if (!wifiManager.setCredentials(ssid, password, token, &tokenIncluded)) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      sendWifiStatus(sequence, tokenIncluded, token);
      return;
    }

    case GET_WIFI_STATUS:
      if (length != 0) sendNack(sequence, command, INVALID_PAYLOAD);
      else sendWifiStatus(sequence, false, nullptr);
      return;

    case CLEAR_WIFI_CREDENTIALS:
      if (length != 0) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        // A station-side WebSocket may be carrying this command.
        sendAck(sequence, command);
        wifiManager.clearCredentials();
      }
      return;

    case SET_WIFI_ENABLED:
      if (length != 1) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        const bool enabled = payload[0] != 0;
        if (!enabled) sendAck(sequence, command);
        wifiManager.setEnabled(enabled);
        if (enabled) sendAck(sequence, command);
      }
      return;

    case SET_HOTSPOT_CONFIG: {
      if (currentReplyTarget != ReplyTarget::kSerial || length < 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      const uint8_t mode = payload[0];
      const uint8_t passwordLength = payload[1];
      if (!isValidHotspotMode(mode) ||
          length != static_cast<uint32_t>(2 + passwordLength) ||
          (passwordLength != 0 &&
           !isValidHotspotPasswordLength(passwordLength))) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      char password[kHotspotMaxPasswordLength + 1] = {};
      memcpy(password, payload + 2, passwordLength);
      if (!wifiManager.setHotspotConfig(mode, password, passwordLength)) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        sendHotspotStatus(sequence);
      }
      return;
    }

    case GET_HOTSPOT_STATUS:
      if (length != 0) sendNack(sequence, command, INVALID_PAYLOAD);
      else sendHotspotStatus(sequence);
      return;

    default:
      sendNack(sequence, command, UNSUPPORTED_COMMAND);
  }
}

class FrameParser {
 public:
  void feed(uint8_t byte) {
    lastByteAt_ = millis();
    switch (state_) {
      case State::MAGIC_0:
        if (byte == kMagic0) state_ = State::MAGIC_1;
        break;
      case State::MAGIC_1:
        if (byte == kMagic1) {
          state_ = State::HEADER;
          headerIndex_ = 0;
          crc_ = 0xFFFF;
        } else if (byte != kMagic0) {
          state_ = State::MAGIC_0;
        }
        break;
      case State::HEADER:
        header_[headerIndex_++] = byte;
        crc_ = crc16CcittFalseUpdate(crc_, byte);
        if (headerIndex_ == sizeof(header_)) {
          version_ = header_[0];
          command_ = header_[1];
          sequence_ = readLe16(header_ + 2);
          payloadLength_ = readLe32(header_ + 4);
          payloadIndex_ = 0;
          if (payloadLength_ > kMaxWirePayload) {
            sendNack(sequence_, command_, INVALID_PAYLOAD);
            reset();
          } else {
            state_ = payloadLength_ ? State::PAYLOAD : State::CRC_0;
          }
        }
        break;
      case State::PAYLOAD:
        payload_[payloadIndex_++] = byte;
        crc_ = crc16CcittFalseUpdate(crc_, byte);
        if (payloadIndex_ == payloadLength_) state_ = State::CRC_0;
        break;
      case State::CRC_0:
        receivedCrc_ = byte;
        state_ = State::CRC_1;
        break;
      case State::CRC_1:
        receivedCrc_ |= static_cast<uint16_t>(byte) << 8;
        dispatch();
        reset();
        break;
    }
  }

  void pollTimeout() {
    if (state_ != State::MAGIC_0 &&
        static_cast<uint32_t>(millis() - lastByteAt_) > kParserTimeoutMs) {
      reset();
    }
  }

 private:
  enum class State { MAGIC_0, MAGIC_1, HEADER, PAYLOAD, CRC_0, CRC_1 };

  void dispatch() {
    if (receivedCrc_ != crc_) {
      sendNack(sequence_, command_, BAD_CRC);
    } else if (version_ != kProtocolVersion) {
      sendNack(sequence_, command_, UNSUPPORTED_VERSION);
    } else {
      handleCommand(command_, sequence_, payload_, payloadLength_);
    }
  }

  void reset() {
    state_ = State::MAGIC_0;
    headerIndex_ = 0;
    payloadIndex_ = 0;
  }

  State state_ = State::MAGIC_0;
  uint8_t header_[8]{};
  uint8_t payload_[kMaxWirePayload]{};
  uint32_t headerIndex_ = 0;
  uint32_t payloadIndex_ = 0;
  uint32_t payloadLength_ = 0;
  uint32_t lastByteAt_ = 0;
  uint16_t sequence_ = 0;
  uint16_t crc_ = 0;
  uint16_t receivedCrc_ = 0;
  uint8_t version_ = 0;
  uint8_t command_ = 0;
};

FrameParser serialParser;
FrameParser wsParser;
}  // namespace

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(kSerialBaud);
  rtc_gpio_deinit(kSleepButton);
  pinMode(kSleepButton, INPUT_PULLUP);
  gpio_hold_dis(static_cast<gpio_num_t>(board::kBacklight));
  display.init();
  display.setBrightness(backlight);
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);

  filesystemMounted = LittleFS.begin(false);
  if (!filesystemMounted) filesystemMounted = LittleFS.begin(true);
  if (filesystemMounted && loadActiveMetadata()) startStoredPlayback();

  wifiManager.begin();
}

void loop() {
  currentReplyTarget = ReplyTarget::kSerial;
  while (Serial.available() > 0) {
    serialParser.feed(static_cast<uint8_t>(Serial.read()));
  }
  serialParser.pollTimeout();

  currentReplyTarget = ReplyTarget::kWebSocket;
  uint8_t wsByte;
  while (wsReadByte(&wsByte)) {
    wsParser.feed(wsByte);
  }
  wsParser.pollTimeout();

  wifiManager.poll();
  wsServerPoll();
  advanceStoredPlayback();
  pollSleepButton();
  yield();
}
