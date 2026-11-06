#include <Arduino.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <LovyanGFX.hpp>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp32-hal-cpu.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>

#include "boot_guard.h"

#include "protocol.h"
#include "visualizer.h"
#include "src/ble_media.h"
#include "src/wifi_manager.h"
#include "src/ws_server.h"

namespace board {
// LilyGO T-Display-S3: ESP32-S3 with a 1.9" ST7789 on an 8-bit i8080 bus.
constexpr int kData[8] = {39, 40, 41, 42, 45, 46, 47, 48};
constexpr int kWr = 8;
constexpr int kRd = 9;
constexpr int kDc = 7;
constexpr int kCs = 6;
constexpr int kReset = 5;
constexpr int kBacklight = 38;
// Gates LCD power; must be driven high when running from the battery.
constexpr int kLcdPower = 15;
// Battery sense through a 2:1 divider (ADC1), and the second user button.
constexpr int kBatteryAdc = 4;
constexpr int kBatteryDivider = 2;
constexpr int kBatteryButton = 14;
constexpr uint16_t kWidth = 170;
constexpr uint16_t kHeight = 320;

constexpr uint16_t kControllerWidth = 240;
constexpr uint16_t kControllerHeight = 320;
constexpr uint16_t kOffsetX = 35;
constexpr uint16_t kOffsetY = 0;
constexpr bool kInvert = true;
constexpr bool kRgbOrder = false;
constexpr uint32_t kBusFrequency = 20000000;
constexpr uint8_t kBacklightPwmChannel = 7;
constexpr char kDeviceName[] = "CyberClip LilyGO T-Display-S3 ST7789";
}  // namespace board

class MatrixDisplay : public lgfx::LGFX_Device {
 public:
  MatrixDisplay() {
    {
      auto cfg = bus_.config();
      cfg.freq_write = board::kBusFrequency;
      cfg.pin_wr = board::kWr;
      cfg.pin_rd = board::kRd;
      cfg.pin_rs = board::kDc;
      cfg.pin_d0 = board::kData[0];
      cfg.pin_d1 = board::kData[1];
      cfg.pin_d2 = board::kData[2];
      cfg.pin_d3 = board::kData[3];
      cfg.pin_d4 = board::kData[4];
      cfg.pin_d5 = board::kData[5];
      cfg.pin_d6 = board::kData[6];
      cfg.pin_d7 = board::kData[7];
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
  lgfx::Bus_Parallel8 bus_;
  lgfx::Panel_ST7789 panel_;
  lgfx::Light_PWM light_;
};

using namespace cyberclip;

namespace {
constexpr uint32_t kSerialBaud = 921600;
constexpr uint32_t kCpuFrequencyMhz = 160;
// Battery-only clock for a static screen (WiFi still needs at least 80 MHz).
constexpr uint32_t kReducedCpuFrequencyMhz = 80;
// Time without protocol bytes before the CPU may drop to the reduced clock.
constexpr uint32_t kFullSpeedHoldMs = 3000;
constexpr uint32_t kPowerCheckIntervalMs = 5000;
constexpr uint32_t kMaxFrameSize = 128 * 1024;
constexpr uint32_t kConservativeStoredBytes = 8 * 1024 * 1024;
constexpr uint32_t kParserTimeoutMs = 1000;
constexpr uint16_t kMinimumFrameDelayMs = 10;
constexpr gpio_num_t kSleepButton = GPIO_NUM_0;
// On the T-Display-S3, GPIO14 shows the next saved item when tapped and the
// battery overlay when held (see pollBatteryButton).
constexpr gpio_num_t kButtons[] = {kSleepButton};
constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);
constexpr uint32_t kSleepButtonDebounceMs = 30;
constexpr uint32_t kHotspotButtonHoldMs = 1500;
constexpr uint8_t kFirmwareMajor = 2;
constexpr uint8_t kFirmwareMinor = 0;
constexpr uint8_t kFirmwarePatch = 29;
using board::kDeviceName;
// Before the saved media library, the board kept one item: /playlist.meta
// with frames in /aNNN.jpg or /bNNN.jpg. It is moved into slot 0 on boot.
constexpr char kLegacyMetadataPath[] = "/playlist.meta";
constexpr char kLegacyMetadataTempPath[] = "/playlist.tmp";
constexpr char kActiveSlotPath[] = "/active";
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
  uint16_t delays[kMaxStoredFrames]{};
  uint16_t frameCount = 0;
  uint16_t storedCount = 0;
  uint32_t bytes = 0;
  uint32_t itemId = 0;
  uint8_t mediaType = 0;
  uint8_t rotation = 0;
  uint8_t slot = 0;
  bool loop = false;
  bool valid = false;
} activePlaylist, stagingPlaylist;

// Summary of each saved item, read from its metadata at boot and kept up to
// date as items are added and deleted, so listing never touches the flash.
struct StoredItem {
  uint32_t bytes = 0;
  uint32_t itemId = 0;
  uint16_t frameCount = 0;
  uint8_t mediaType = 0;
  uint8_t rotation = 0;
  bool loop = false;
  bool used = false;
} storedItems[kMaxStoredItems];
uint8_t activeSlot = kNoStoredSlot;

struct Playback {
  uint16_t nextFrame = 0;
  uint32_t deadline = 0;
  bool running = false;
  // Late frames in a row at the reduced clock, and whether this GIF has
  // fallen behind there and now plays at full speed.
  uint8_t lateFrames = 0;
  bool needsFullSpeed = false;
} playback;

struct UploadProgress {
  int16_t x = 0;
  int16_t y = 0;
  int16_t innerWidth = 0;
  uint16_t filledPixels = UINT16_MAX;
  bool visible = false;
} uploadProgress;

// Frames are decoded off-screen and pushed to the panel in one burst, so the
// panel never scans out a half-drawn frame (tearing). The buffer is kept in the
// panel's native portrait orientation and rotated frames are rotated while
// decoding: the panel refreshes row by row in that orientation, and writing in
// any other order makes the boundary between old and new frames diagonal. The
// buffer is split into bands so it still fits when the heap is fragmented.
constexpr uint8_t kFramebufferBands = 4;
constexpr uint16_t kFramebufferBandRows =
    (board::kHeight + kFramebufferBands - 1) / kFramebufferBands;
constexpr uint32_t kFramebufferBandPixels =
    static_cast<uint32_t>(kFramebufferBandRows) * board::kWidth;

struct Framebuffer {
  uint16_t *bands[kFramebufferBands]{};
  // Rotation of the decoded frame, and its size in that rotation.
  uint8_t rotation = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool allocated = false;
  // Set when the buffer holds a decoded stored frame that is not yet shown.
  bool holdsPlaylistFrame = false;
  // Set while the panel shows exactly the buffer's contents, so an overlay
  // can be removed by pushing the buffer again.
  bool presented = false;
  uint8_t slot = 0;
  uint16_t frameIndex = 0;
} framebuffer;

enum class JpegOutput : uint8_t { kDiscard, kDisplay, kFramebuffer };
JpegOutput jpegOutput = JpegOutput::kDiscard;

uint8_t backlight = 128;
bool filesystemMounted = false;
struct ButtonState {
  bool armed = false;
  bool pressed = false;
  bool longHandled = false;
  uint32_t pressedAt = 0;
} buttonStates[kButtonCount];

// Selects where writeFrame() sends its next reply. Set immediately before
// feeding bytes into serialParser/wsParser in loop(), so it is always
// correct for the synchronous sendAck/sendNack/sendHello/sendStatus calls
// that happen while handling that byte.
enum class ReplyTarget { kSerial, kWebSocket };
ReplyTarget currentReplyTarget = ReplyTarget::kSerial;

bool startStoredPlayback();
bool decodeJpegBuffer(uint8_t *data, uint32_t size, uint16_t width,
                      uint16_t height, uint8_t rotation, bool render);
bool renderPlaylistFrame(const Playlist &playlist, uint16_t frameIndex);
void presentFramebuffer();
void drawOverlays();
void drawHotspotQr();
void cancelHotspotFromQr();
void restoreUnderOverlay(int16_t x, int16_t y, int16_t width, int16_t height,
                         uint8_t rotation);

// Tracks what is on screen so a temporary overlay can be removed afterwards.
enum class ScreenContent : uint8_t { kFill, kPlaylistFrame, kLiveFrame, kProgress };
struct ScreenState {
  ScreenContent content = ScreenContent::kFill;
  uint16_t fillColor = 0;
  uint8_t slot = 0;
  uint16_t playlistFrame = 0;
} screen;

// Full-screen now-playing view (kNowPlayingFullScreen). While it is up,
// `screen` keeps describing the media underneath, which comes back when the
// music stops, and stored playback is paused.
struct {
  bool visible = false;
  uint8_t rotation = 0;
  // Progress bar geometry from the last full draw, so the bar and times can
  // be updated without redrawing the rest.
  bool hasProgress = false;
  int16_t barX = 0;
  int16_t barY = 0;
  int16_t barWidth = 0;
  int16_t timesY = 0;
  int32_t shownSecond = -1;
  int16_t shownFill = -1;
  // Wave area from the last full draw, and the per-song look.
  int16_t waveX = 0;
  int16_t waveY = 0;
  int16_t waveWidth = 0;
  uint32_t waveDrawnAt = 0;
  visualizer::WaveParams wave;
  uint16_t color = 0x05ff;
} nowPlayingScreen;

// Stored GIF playback that is actually advancing on the panel.
// Full-screen hotspot QR codes, drawn over whatever the screen holds (which
// is redrawn when they hide). See HotspotQrStage in protocol.h.
struct HotspotQr {
  HotspotQrStage stage = HotspotQrStage::kHidden;
  uint32_t stageAt = 0;
  uint8_t rotation = 0;  // Rotation of the screen underneath.
  bool visible() const { return stage != HotspotQrStage::kHidden; }
} hotspotQr;

bool playbackActive() {
  return playback.running && !nowPlayingScreen.visible && !hotspotQr.visible();
}

constexpr uint32_t kBatteryOverlayMs = 3000;
constexpr uint8_t kBatterySamples = 16;
constexpr int16_t kBatteryOverlayMargin = 4;
constexpr int16_t kBatteryOverlayWidth = 66;
constexpr int16_t kBatteryOverlayHeight = 22;

// Last live (non-stored) frame, kept so it can be redrawn under the overlay.
struct LiveFrame {
  uint8_t *data = nullptr;
  uint32_t size = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t rotation = 0;
} liveFrame;

struct BatteryOverlay {
  bool visible = false;
  uint32_t shownAt = 0;
  uint32_t millivolts = 0;
  uint8_t rotation = 0;
} batteryOverlay;

struct {
  bool armed = false;
  bool pressed = false;
  bool longHandled = false;
  uint32_t pressedAt = 0;
} batteryButton;

// "2 of 3" in the top-left corner after a tap moves to another saved item.
constexpr uint32_t kItemHintMs = 2000;
constexpr int16_t kItemHintMargin = 4;
constexpr int16_t kItemHintHeight = 22;
struct {
  bool visible = false;
  uint32_t shownAt = 0;
  uint8_t rotation = 0;
  int16_t width = 0;
  char text[20] = {};
} itemHint;

void showNextStoredItem();

void releaseLiveFrame() {
  free(liveFrame.data);
  liveFrame = LiveFrame{};
}

uint32_t readBatteryMillivolts() {
  uint32_t total = 0;
  for (uint8_t i = 0; i < kBatterySamples; ++i) {
    total += analogReadMilliVolts(board::kBatteryAdc);
  }
  return total / kBatterySamples * board::kBatteryDivider;
}

int16_t batteryOverlayX() {
  return display.width() - kBatteryOverlayWidth - kBatteryOverlayMargin;
}

void drawBatteryOverlay() {
  batteryOverlay.rotation = display.getRotation();
  const int16_t x = batteryOverlayX();
  const int16_t y = kBatteryOverlayMargin;
  const bool externalPower =
      batteryOverlay.millivolts >= kBatteryExternalPowerMillivolts;
  const uint8_t percent = batteryPercentFromMillivolts(batteryOverlay.millivolts);
  const BatteryLevel level =
      externalPower ? BATTERY_HIGH : batteryLevelFromPercent(percent);
  const uint16_t levelColor = level == BATTERY_HIGH     ? TFT_GREEN
                              : level == BATTERY_MEDIUM ? TFT_YELLOW
                                                        : TFT_RED;

  display.startWrite();
  display.fillRoundRect(x, y, kBatteryOverlayWidth, kBatteryOverlayHeight, 4,
                        TFT_BLACK);
  display.drawRoundRect(x, y, kBatteryOverlayWidth, kBatteryOverlayHeight, 4,
                        TFT_DARKGREY);

  constexpr int16_t kBodyWidth = 24;
  constexpr int16_t kBodyHeight = 12;
  constexpr int16_t kSegmentWidth = 6;
  const int16_t bodyX = x + 5;
  const int16_t bodyY = y + (kBatteryOverlayHeight - kBodyHeight) / 2;
  display.drawRect(bodyX, bodyY, kBodyWidth, kBodyHeight, TFT_WHITE);
  display.fillRect(bodyX + kBodyWidth, bodyY + 3, 2, kBodyHeight - 6, TFT_WHITE);
  for (uint8_t i = 0; i < 3; ++i) {
    display.fillRect(bodyX + 2 + i * (kSegmentWidth + 1), bodyY + 2,
                     kSegmentWidth, kBodyHeight - 4,
                     i < level ? levelColor : TFT_BLACK);
  }

  char label[8];
  if (externalPower) snprintf(label, sizeof(label), "USB");
  else snprintf(label, sizeof(label), "%u%%", static_cast<unsigned>(percent));
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_left);
  display.drawString(label, bodyX + kBodyWidth + 6, y + kBatteryOverlayHeight / 2);
  display.setTextDatum(top_left);
  display.endWrite();
}

void showBatteryOverlay() {
  if (screen.content == ScreenContent::kProgress) return;
  batteryOverlay.millivolts = readBatteryMillivolts();
  batteryOverlay.visible = true;
  batteryOverlay.shownAt = millis();
  drawBatteryOverlay();
}

void hideBatteryOverlay() {
  if (!batteryOverlay.visible) return;
  batteryOverlay.visible = false;
  restoreUnderOverlay(batteryOverlayX(), kBatteryOverlayMargin,
                      kBatteryOverlayWidth, kBatteryOverlayHeight,
                      batteryOverlay.rotation);
}

void pollBatteryButton() {
  const bool pressed = digitalRead(board::kBatteryButton) == LOW;
  if (!batteryButton.armed) {
    if (!pressed) batteryButton.armed = true;
    return;
  }
  if (pressed && !batteryButton.pressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(board::kBatteryButton) != LOW) return;
    batteryButton.pressed = true;
    batteryButton.longHandled = false;
    batteryButton.pressedAt = millis();
    return;
  }
  // Held: the battery indicator, which stays up until the button is let go.
  if (pressed && batteryButton.pressed && !batteryButton.longHandled &&
      isBatteryButtonHeld(batteryButton.pressedAt, millis())) {
    batteryButton.longHandled = true;
    showBatteryOverlay();
    return;
  }
  if (!pressed && batteryButton.pressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(board::kBatteryButton) == LOW) return;
    batteryButton.pressed = false;
    // A tap: the next saved item.
    if (!batteryButton.longHandled) {
      // A tap on a hotspot QR screen undoes the BOOT hold instead.
      if (hotspotQr.visible()) cancelHotspotFromQr();
      else showNextStoredItem();
    }
    batteryButton.longHandled = false;
  }
}

void pollBatteryOverlay() {
  // The overlay stays up while the button is held.
  if (batteryOverlay.visible && !batteryButton.pressed &&
      millis() - batteryOverlay.shownAt >= kBatteryOverlayMs) {
    hideBatteryOverlay();
  }
}

void drawItemHint() {
  itemHint.rotation = display.getRotation();
  display.startWrite();
  display.fillRoundRect(kItemHintMargin, kItemHintMargin, itemHint.width,
                        kItemHintHeight, 4, TFT_BLACK);
  display.drawRoundRect(kItemHintMargin, kItemHintMargin, itemHint.width,
                        kItemHintHeight, 4, TFT_DARKGREY);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_center);
  display.drawString(itemHint.text, kItemHintMargin + itemHint.width / 2,
                     kItemHintMargin + kItemHintHeight / 2);
  display.setTextDatum(top_left);
  display.endWrite();
}

void showItemHint(const char *text) {
  if (screen.content == ScreenContent::kProgress) return;
  snprintf(itemHint.text, sizeof(itemHint.text), "%s", text);
  display.setTextSize(1);
  itemHint.width = static_cast<int16_t>(display.textWidth(itemHint.text) + 14);
  itemHint.visible = true;
  itemHint.shownAt = millis();
  drawItemHint();
}

void hideItemHint() {
  if (!itemHint.visible) return;
  itemHint.visible = false;
  restoreUnderOverlay(kItemHintMargin, kItemHintMargin, itemHint.width,
                      kItemHintHeight, itemHint.rotation);
}

void pollItemHint() {
  if (itemHint.visible && millis() - itemHint.shownAt >= kItemHintMs) {
    hideItemHint();
  }
}

struct {
  bool onBattery = false;
  bool fullSpeed = true;
  bool checked = false;
  uint32_t checkedAt = 0;
  uint32_t lastActivityAt = 0;
  // Last time something was connected over WiFi (or WiFi came back on).
  uint32_t lastWifiConnectionAt = 0;
} power;

void setFullSpeed(bool fullSpeed) {
  if (power.fullSpeed == fullSpeed) return;
  setCpuFrequencyMhz(fullSpeed ? kCpuFrequencyMhz : kReducedCpuFrequencyMhz);
  power.fullSpeed = fullSpeed;
}

// The panel shows *backlight* (the level the app set), halved on battery.
void applyBacklight() {
  display.setBrightness(backlightLevelFor(power.onBattery, backlight));
}

// Called before protocol bytes are handled, so any decode or draw they
// trigger already runs at full speed and WiFi is back before a WiFi command
// is handled. Also restarts the WiFi idle timer.
void noteActivity() {
  power.lastActivityAt = millis();
  setFullSpeed(true);
  wifiManager.resume();
}

void updatePowerMode() {
  const uint32_t now = millis();
  const bool needsFullSpeed =
      transfer.active || batteryOverlay.visible || batteryButton.pressed ||
      (playbackActive() && playback.needsFullSpeed);
  // The ADC read takes well under a millisecond, so it runs only when the
  // next GIF frame is far enough away that it cannot delay it. A GIF that
  // never leaves that slack still gets a read every few intervals.
  const bool hasSlack =
      !transfer.active &&
      loopIdleSleepMs(false, playbackActive(),
                      static_cast<int32_t>(playback.deadline - now)) > 0;
  const uint32_t sinceCheck = now - power.checkedAt;
  if (!power.checked ||
      (hasSlack && sinceCheck >= kPowerCheckIntervalMs) ||
      sinceCheck >= kPowerCheckIntervalMs * 6) {
    const bool wasOnBattery = power.onBattery;
    power.onBattery = onBatteryPower(power.onBattery, readBatteryMillivolts());
    if (power.onBattery != wasOnBattery) {
      applyBacklight();
      // Plugging in or unplugging restarts the WiFi idle timer and brings
      // WiFi back if it was off.
      if (power.checked) {
        power.lastActivityAt = now;
        wifiManager.resume();
      }
    }
    power.checked = true;
    power.checkedAt = now;
  }
  setFullSpeed(shouldRunAtFullSpeed(power.onBattery, needsFullSpeed,
                                    now - power.lastActivityAt,
                                    kFullSpeedHoldMs));

  // While WiFi is off (or the hotspot QR codes are up for someone joining)
  // the no-connection clock does not run, so it starts fresh when WiFi
  // comes back on.
  const bool wifiConnected =
      wifiManager.hotspotStationCount() > 0 || wsHasClient() ||
      wsHasActiveApClientOrTransfer();
  if (wifiManager.suspended() || wifiConnected || hotspotQr.visible()) {
    power.lastWifiConnectionAt = now;
  }

  if (shouldSuspendWifi(power.onBattery, transfer.active,
                        now - power.lastActivityAt, kWifiIdleTimeoutMs) ||
      shouldSuspendWifiWithoutConnection(
          wifiConnected, now - power.lastWifiConnectionAt,
          kWifiNoConnectionTimeoutMs)) {
    wifiManager.suspend();
  }
}

// What the paired iPhone is playing (src/ble_media.cpp) can be shown in
// three ways. Each is a hardcoded switch; they can be combined. All of them
// need the CYBERCLIP_BLE_MEDIA build flag (see platformio.ini).
//
// Banner with the battery indicator while GPIO14 is pressed.
constexpr bool kNowPlayingOnButton = true;
// Replace the photo or GIF with a now-playing screen while music plays. The
// media comes back (and a GIF resumes) when playback pauses or stops.
constexpr bool kNowPlayingFullScreen = true;
// Banner for a few seconds whenever a track starts or playback resumes.
constexpr bool kNowPlayingAutoBanner = false;

constexpr uint32_t kNowPlayingOverlayMs = 6000;
// iOS sends the title and artist as separate notifications; waiting briefly
// keeps the screen from showing the new title with the old artist.
constexpr uint32_t kNowPlayingSettleMs = 300;
constexpr int16_t kNowPlayingMargin = 4;
constexpr int16_t kNowPlayingHeight = 34;
constexpr int16_t kNowPlayingIconWidth = 18;
constexpr uint16_t kNowPlayingArtistColor = 0xBDF7;  // light grey
const lgfx::IFont *const kNowPlayingFont = &fonts::efontJA_12;
const lgfx::IFont *const kNowPlayingTitleFont = &fonts::efontJA_16;
constexpr uint8_t kNowPlayingScreenIconScale = 3;
constexpr uint8_t kNowPlayingScreenTitleLines = 3;
constexpr uint8_t kNowPlayingScreenArtistLines = 2;
constexpr int16_t kNowPlayingScreenPadding = 10;
constexpr int16_t kNowPlayingBarHeight = 4;
// Animated sine/cosine wave instead of the note icon on the full screen.
constexpr bool kNowPlayingWave = true;
constexpr int16_t kWaveHeight = 40;
constexpr int16_t kWaveBarWidth = 4;
constexpr int16_t kWaveBarGap = 2;
constexpr uint32_t kWaveFrameMs = 33;  // about 30 frames per second
constexpr uint16_t kNowPlayingBarTrackColor = 0x39E7;  // dark grey
constexpr size_t kFittedTextSize = kNowPlayingTextSize + 3;

struct {
  bool visible = false;
  // Shown by the GPIO14 button, so it hides together with the battery
  // indicator rather than on its own timer.
  bool onButton = false;
  bool pending = false;
  uint32_t seenRevision = 0;
  uint32_t changedAt = 0;
  uint32_t shownAt = 0;
  uint8_t rotation = 0;
  NowPlaying latest;
  // What was last announced, so unrelated updates do not re-show it.
  char shownTitle[kNowPlayingTextSize] = {};
  char shownArtist[kNowPlayingTextSize] = {};
  bool wasPlaying = false;
  bool buttonWasPressed = false;
} nowPlayingOverlay;

bool nowPlayingIsPlaying(const NowPlaying &info) {
  return info.connected && info.playback == ams::PLAYBACK_PLAYING &&
         info.title[0] != '\0';
}

int16_t nowPlayingOverlayY() {
  return display.height() - kNowPlayingHeight - kNowPlayingMargin;
}

int16_t nowPlayingOverlayWidth() {
  return display.width() - kNowPlayingMargin * 2;
}

// Copies *text* into *out*, ending it with "..." if it is wider than
// *maxWidth* in the current font. Cuts only between UTF-8 characters.
void fitText(const char *text, size_t length, int16_t maxWidth, char *out) {
  length = ams::utf8Boundary(text, length, kNowPlayingTextSize - 1);
  memcpy(out, text, length);
  out[length] = '\0';
  if (display.textWidth(out) <= maxWidth) return;
  const int16_t ellipsisWidth = display.textWidth("...");
  while (length > 0) {
    length = ams::utf8Boundary(text, length, length - 1);
    out[length] = '\0';
    if (display.textWidth(out) + ellipsisWidth <= maxWidth) break;
  }
  memcpy(out + length, "...", 4);
}

void drawFittedText(const char *text, int16_t x, int16_t y, int16_t maxWidth) {
  char fitted[kFittedTextSize];
  fitText(text, strlen(text), maxWidth, fitted);
  display.drawString(fitted, x, y);
}

// Splits *text* into at most *maxLines* lines no wider than *maxWidth*,
// breaking at spaces where possible; the last line ends with "..." if text
// is left over. Returns the number of lines.
uint8_t wrapText(const char *text, int16_t maxWidth, uint8_t maxLines,
                 char lines[][kFittedTextSize]) {
  const size_t length = strlen(text);
  size_t position = 0;
  uint8_t count = 0;
  char candidate[kFittedTextSize];
  while (position < length && count < maxLines) {
    while (position < length && text[position] == ' ') ++position;
    if (position >= length) break;
    const char *rest = text + position;
    const size_t restLength = length - position;
    if (count + 1 == maxLines) {
      fitText(rest, restLength, maxWidth, lines[count++]);
      break;
    }
    // Longest prefix that fits, and the last space within it.
    size_t fits = 0;
    size_t lastSpace = 0;
    for (size_t next = ams::utf8Next(rest, restLength, 0);;
         next = ams::utf8Next(rest, restLength, next)) {
      if (next >= kFittedTextSize) break;
      memcpy(candidate, rest, next);
      candidate[next] = '\0';
      if (display.textWidth(candidate) > maxWidth) break;
      fits = next;
      if (next < restLength && rest[next] == ' ') lastSpace = next;
      if (next >= restLength) break;
    }
    if (fits == 0) fits = ams::utf8Next(rest, restLength, 0);
    const size_t take =
        (fits < restLength && lastSpace > 0) ? lastSpace : fits;
    memcpy(lines[count], rest, take);
    lines[count++][take] = '\0';
    position += take;
  }
  return count;
}

// A beamed pair of eighth notes, (12 * scale + scale) x (15 * scale).
void drawNoteIcon(int16_t x, int16_t y, uint8_t scale, uint16_t color) {
  const int16_t s = scale;
  display.fillCircle(x + 2 * s, y + 13 * s, 2 * s, color);
  display.fillCircle(x + 10 * s, y + 11 * s, 2 * s, color);
  display.fillRect(x + 4 * s, y + 2 * s, s, 11 * s, color);
  display.fillRect(x + 12 * s, y, s, 11 * s, color);
  display.fillTriangle(x + 4 * s, y + 2 * s, x + 13 * s, y, x + 13 * s,
                       y + 2 * s, color);
  display.fillTriangle(x + 4 * s, y + 2 * s, x + 13 * s, y + 2 * s, x + 4 * s,
                       y + 4 * s, color);
}

void drawNowPlayingOverlay() {
  nowPlayingOverlay.rotation = display.getRotation();
  const NowPlaying &info = nowPlayingOverlay.latest;
  const char *title = info.title;
  const char *artist = info.artist;
  if (!info.connected) {
    title = "iPhone not connected";
    artist = "Pair in Settings > Bluetooth";
  } else if (title[0] == '\0') {
    title = "Nothing playing";
    artist = "";
  }
  const int16_t x = kNowPlayingMargin;
  const int16_t y = nowPlayingOverlayY();
  const int16_t width = nowPlayingOverlayWidth();

  display.startWrite();
  display.fillRoundRect(x, y, width, kNowPlayingHeight, 4, TFT_BLACK);
  display.drawRoundRect(x, y, width, kNowPlayingHeight, 4, TFT_DARKGREY);
  drawNoteIcon(x + 4, y + (kNowPlayingHeight - 15) / 2, 1, TFT_WHITE);

  const int16_t textX = x + 4 + kNowPlayingIconWidth;
  const int16_t textWidth = width - (textX - x) - 4;
  display.setFont(kNowPlayingFont);
  display.setTextSize(1);
  display.setTextDatum(top_left);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  drawFittedText(title, textX, y + 4, textWidth);
  display.setTextColor(kNowPlayingArtistColor, TFT_BLACK);
  drawFittedText(artist, textX, y + 18, textWidth);
  display.setFont(&fonts::Font0);
  display.endWrite();
}

void showNowPlayingOverlay(bool onButton) {
  if (screen.content == ScreenContent::kProgress) return;
  nowPlayingOverlay.visible = true;
  nowPlayingOverlay.onButton = onButton;
  nowPlayingOverlay.shownAt = millis();
  // The full-screen view already shows the same information.
  if (!nowPlayingScreen.visible) drawNowPlayingOverlay();
}

void hideNowPlayingOverlay() {
  nowPlayingOverlay.onButton = false;
  if (!nowPlayingOverlay.visible) return;
  nowPlayingOverlay.visible = false;
  if (nowPlayingScreen.visible) return;
  display.setRotation(nowPlayingOverlay.rotation);
  restoreUnderOverlay(kNowPlayingMargin, nowPlayingOverlayY(),
                      nowPlayingOverlayWidth(), kNowPlayingHeight,
                      nowPlayingOverlay.rotation);
}

float nowPlayingElapsed(const NowPlaying &info) {
  const bool advancing = info.playback == ams::PLAYBACK_PLAYING ||
                         info.playback == ams::PLAYBACK_REWINDING ||
                         info.playback == ams::PLAYBACK_FAST_FORWARDING;
  ams::PlaybackInfo position = info.position;
  if (!advancing) position.rate = 0;
  return ams::extrapolateElapsed(position, millis() - info.positionAtMs,
                                 info.durationSeconds);
}

// Redraws the bar and times when the second or the filled width changed.
void drawNowPlayingProgress(bool force) {
  const NowPlaying &info = nowPlayingOverlay.latest;
  if (!nowPlayingScreen.hasProgress || info.durationSeconds <= 0) return;
  const float elapsed = nowPlayingElapsed(info);
  const int32_t second = static_cast<int32_t>(elapsed);
  const int16_t fill = static_cast<int16_t>(
      nowPlayingScreen.barWidth * (elapsed / info.durationSeconds));
  if (!force && second == nowPlayingScreen.shownSecond &&
      fill == nowPlayingScreen.shownFill) {
    return;
  }
  nowPlayingScreen.shownSecond = second;
  nowPlayingScreen.shownFill = fill;

  char elapsedText[12];
  char durationText[12];
  ams::formatTrackTime(elapsed, elapsedText, sizeof(elapsedText));
  ams::formatTrackTime(info.durationSeconds, durationText,
                       sizeof(durationText));

  display.setRotation(nowPlayingScreen.rotation);
  display.startWrite();
  const int16_t x = nowPlayingScreen.barX;
  const int16_t y = nowPlayingScreen.barY;
  const int16_t width = nowPlayingScreen.barWidth;
  display.fillRect(x, y, fill, kNowPlayingBarHeight, nowPlayingScreen.color);
  display.fillRect(x + fill, y, width - fill, kNowPlayingBarHeight,
                   kNowPlayingBarTrackColor);
  display.setFont(kNowPlayingFont);
  display.setTextSize(1);
  display.fillRect(x, nowPlayingScreen.timesY, width, display.fontHeight(),
                   TFT_BLACK);
  display.setTextColor(kNowPlayingArtistColor, TFT_BLACK);
  display.setTextDatum(top_left);
  display.drawString(elapsedText, x, nowPlayingScreen.timesY);
  display.setTextDatum(top_right);
  display.drawString(durationText, x + width, nowPlayingScreen.timesY);
  display.setTextDatum(top_left);
  display.setFont(&fonts::Font0);
  display.endWrite();
}

// New random wave shape and color, once per song.
void randomizeNowPlayingLook() {
  uint32_t random[6];
  for (uint32_t &value : random) value = esp_random();
  nowPlayingScreen.wave = visualizer::randomWaveParams(random);
  nowPlayingScreen.color =
      visualizer::randomSongColor(esp_random(), esp_random());
}

// Draws one frame of the wave. Each bar is redrawn in place (bar, then the
// black above and below it), so nothing is cleared and nothing flickers.
void drawNowPlayingWave() {
  nowPlayingScreen.waveDrawnAt = millis();
  const int16_t barCount =
      (nowPlayingScreen.waveWidth + kWaveBarGap) / (kWaveBarWidth + kWaveBarGap);
  if (barCount <= 0) return;
  const int16_t usedWidth =
      barCount * (kWaveBarWidth + kWaveBarGap) - kWaveBarGap;
  const int16_t startX =
      nowPlayingScreen.waveX + (nowPlayingScreen.waveWidth - usedWidth) / 2;
  const int16_t top = nowPlayingScreen.waveY;
  const float seconds = nowPlayingScreen.waveDrawnAt / 1000.0f;

  // Bars under the battery indicator are skipped while it is up.
  const int16_t batteryLeft = batteryOverlayX() - 1;
  const int16_t batteryBottom = kBatteryOverlayMargin + kBatteryOverlayHeight;
  const bool batteryCovers = batteryOverlay.visible && top < batteryBottom;

  display.setRotation(nowPlayingScreen.rotation);
  display.startWrite();
  for (int16_t i = 0; i < barCount; ++i) {
    const int16_t x = startX + i * (kWaveBarWidth + kWaveBarGap);
    if (batteryCovers && x + kWaveBarWidth > batteryLeft) continue;
    const float position = barCount > 1 ? i / float(barCount - 1) : 0.5f;
    const float level =
        visualizer::waveLevel(nowPlayingScreen.wave, seconds, position);
    int16_t height = static_cast<int16_t>(level * kWaveHeight);
    height = constrain(height, 2, kWaveHeight);
    const int16_t barTop = top + (kWaveHeight - height) / 2;
    display.fillRect(x, top, kWaveBarWidth, barTop - top, TFT_BLACK);
    display.fillRect(x, barTop, kWaveBarWidth, height, nowPlayingScreen.color);
    display.fillRect(x, barTop + height, kWaveBarWidth,
                     top + kWaveHeight - barTop - height, TFT_BLACK);
  }
  display.endWrite();
}

void drawNowPlayingScreen() {
  display.setRotation(nowPlayingScreen.rotation);
  const NowPlaying &info = nowPlayingOverlay.latest;
  const int16_t width = display.width();
  const int16_t textWidth = width - kNowPlayingScreenPadding * 2;
  char titleLines[kNowPlayingScreenTitleLines][kFittedTextSize];
  char artistLines[kNowPlayingScreenArtistLines][kFittedTextSize];
  const bool hasProgress = info.durationSeconds > 0;

  display.setTextSize(1);
  display.setFont(kNowPlayingTitleFont);
  const int16_t titleLineHeight = display.fontHeight() + 2;
  display.setFont(kNowPlayingFont);
  const int16_t smallLineHeight = display.fontHeight() + 2;

  constexpr int16_t kIconWidth = 13 * kNowPlayingScreenIconScale;
  constexpr int16_t kIconHeight =
      kNowPlayingWave ? kWaveHeight : 15 * kNowPlayingScreenIconScale;
  constexpr int16_t kIconGap = 14;
  constexpr int16_t kArtistGap = 6;
  constexpr int16_t kProgressGap = 12;
  const int16_t progressHeight =
      hasProgress ? kProgressGap + kNowPlayingBarHeight + 4 + smallLineHeight
                  : 0;

  // Fewer lines in landscape, where everything does not fit.
  uint8_t titleCount = 0;
  uint8_t artistCount = 0;
  int16_t blockHeight = 0;
  for (uint8_t titleMax = kNowPlayingScreenTitleLines; titleMax > 0;
       --titleMax) {
    const uint8_t artistMax =
        titleMax == kNowPlayingScreenTitleLines ? kNowPlayingScreenArtistLines
                                                : 1;
    display.setFont(kNowPlayingTitleFont);
    titleCount = wrapText(info.title, textWidth, titleMax, titleLines);
    display.setFont(kNowPlayingFont);
    artistCount = wrapText(info.artist, textWidth, artistMax, artistLines);
    blockHeight = kIconHeight + kIconGap + titleCount * titleLineHeight +
                  (artistCount ? kArtistGap : 0) +
                  artistCount * smallLineHeight + progressHeight;
    if (blockHeight <= display.height() - 8) break;
  }
  int16_t y = max<int16_t>(4, (display.height() - blockHeight) / 2);

  display.startWrite();
  display.fillScreen(TFT_BLACK);
  const int16_t iconY = y;
  if (!kNowPlayingWave) {
    drawNoteIcon((width - kIconWidth) / 2, y, kNowPlayingScreenIconScale,
                 TFT_WHITE);
  }
  y += kIconHeight + kIconGap;
  display.setTextDatum(top_center);
  display.setFont(kNowPlayingTitleFont);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint8_t i = 0; i < titleCount; ++i, y += titleLineHeight) {
    display.drawString(titleLines[i], width / 2, y);
  }
  if (artistCount) y += kArtistGap;
  display.setFont(kNowPlayingFont);
  display.setTextColor(kNowPlayingArtistColor, TFT_BLACK);
  for (uint8_t i = 0; i < artistCount; ++i, y += smallLineHeight) {
    display.drawString(artistLines[i], width / 2, y);
  }
  display.setTextDatum(top_left);
  display.setFont(&fonts::Font0);
  display.endWrite();

  if (kNowPlayingWave) {
    nowPlayingScreen.waveX = kNowPlayingScreenPadding;
    nowPlayingScreen.waveY = iconY;
    nowPlayingScreen.waveWidth = textWidth;
    drawNowPlayingWave();
  }

  nowPlayingScreen.hasProgress = hasProgress;
  if (hasProgress) {
    nowPlayingScreen.barX = kNowPlayingScreenPadding;
    nowPlayingScreen.barY = y + kProgressGap;
    nowPlayingScreen.barWidth = textWidth;
    nowPlayingScreen.timesY = nowPlayingScreen.barY + kNowPlayingBarHeight + 4;
    drawNowPlayingProgress(true);
  }
}

void showNowPlayingScreen() {
  // Uploads keep their progress screen.
  if (screen.content == ScreenContent::kProgress || transfer.active) return;
  if (hotspotQr.visible()) return;
  if (!nowPlayingScreen.visible) {
    nowPlayingScreen.rotation = display.getRotation();
    nowPlayingScreen.visible = true;
  }
  drawNowPlayingScreen();
  drawOverlays();
}

void hideNowPlayingScreen() {
  if (!nowPlayingScreen.visible) return;
  nowPlayingScreen.visible = false;
  display.setRotation(nowPlayingScreen.rotation);
  restoreUnderOverlay(0, 0, display.width(), display.height(),
                      nowPlayingScreen.rotation);
}

#if CYBERCLIP_BLE_MEDIA
// Runs after pollBatteryOverlay(), so the button banner appears and hides
// together with the battery indicator.
void pollNowPlayingButton() {
  const bool pressed = batteryButton.pressed;
  if (pressed && !nowPlayingOverlay.buttonWasPressed &&
      batteryOverlay.visible) {
    nowPlayingOverlay.latest = bleMedia.snapshot();
    showNowPlayingOverlay(true);
  } else if (nowPlayingOverlay.onButton && !batteryOverlay.visible) {
    hideNowPlayingOverlay();
  }
  nowPlayingOverlay.buttonWasPressed = pressed;
}

void pollNowPlaying() {
  if (kNowPlayingOnButton) pollNowPlayingButton();
  const uint32_t now = millis();
  const uint32_t revision = bleMedia.revision();
  if (revision != nowPlayingOverlay.seenRevision) {
    nowPlayingOverlay.seenRevision = revision;
    nowPlayingOverlay.latest = bleMedia.snapshot();
    const NowPlaying &info = nowPlayingOverlay.latest;
    const bool playing = nowPlayingIsPlaying(info);
    if (!playing) {
      nowPlayingOverlay.pending = false;
      if (kNowPlayingAutoBanner && !nowPlayingOverlay.onButton) {
        hideNowPlayingOverlay();
      }
      if (kNowPlayingFullScreen) hideNowPlayingScreen();
    } else if (!nowPlayingOverlay.wasPlaying ||
               strcmp(info.title, nowPlayingOverlay.shownTitle) != 0 ||
               strcmp(info.artist, nowPlayingOverlay.shownArtist) != 0) {
      nowPlayingOverlay.pending = true;
      nowPlayingOverlay.changedAt = now;
    }
    nowPlayingOverlay.wasPlaying = playing;
    // A button banner follows whatever the iPhone reports while it is up.
    if (nowPlayingOverlay.visible && nowPlayingOverlay.onButton &&
        !nowPlayingScreen.visible) {
      drawNowPlayingOverlay();
    }
  }

  if (nowPlayingScreen.visible && !nowPlayingOverlay.pending) {
    // The track length can arrive after the title and artist.
    if ((nowPlayingOverlay.latest.durationSeconds > 0) !=
        nowPlayingScreen.hasProgress) {
      drawNowPlayingScreen();
      drawOverlays();
    } else {
      drawNowPlayingProgress(false);
      if (kNowPlayingWave &&
          now - nowPlayingScreen.waveDrawnAt >= kWaveFrameMs) {
        drawNowPlayingWave();
      }
    }
  }

  if (nowPlayingOverlay.pending &&
      now - nowPlayingOverlay.changedAt >= kNowPlayingSettleMs) {
    nowPlayingOverlay.pending = false;
    // A different song (not a resume of the same one) gets a new look.
    if (strcmp(nowPlayingOverlay.shownTitle, nowPlayingOverlay.latest.title) !=
            0 ||
        strcmp(nowPlayingOverlay.shownArtist,
               nowPlayingOverlay.latest.artist) != 0) {
      randomizeNowPlayingLook();
    }
    strcpy(nowPlayingOverlay.shownTitle, nowPlayingOverlay.latest.title);
    strcpy(nowPlayingOverlay.shownArtist, nowPlayingOverlay.latest.artist);
    if (kNowPlayingFullScreen) {
      showNowPlayingScreen();
    } else if (kNowPlayingAutoBanner && !nowPlayingOverlay.onButton) {
      showNowPlayingOverlay(false);
    }
  } else if (kNowPlayingAutoBanner && nowPlayingOverlay.visible &&
             !nowPlayingOverlay.onButton &&
             now - nowPlayingOverlay.shownAt >= kNowPlayingOverlayMs) {
    hideNowPlayingOverlay();
  }
}
#else
// Bluetooth is compiled out: nothing ever starts the now-playing view.
void pollNowPlaying() {}
#endif  // CYBERCLIP_BLE_MEDIA

void drawOverlays() {
  if (batteryOverlay.visible) drawBatteryOverlay();
  if (itemHint.visible) drawItemHint();
  if (nowPlayingOverlay.visible && !nowPlayingScreen.visible) {
    drawNowPlayingOverlay();
  }
}

// Redraws the screen after an overlay in *rotation* covering x/y/width/height
// was hidden, then draws the overlays that are still visible.
void restoreUnderOverlay(int16_t x, int16_t y, int16_t width, int16_t height,
                         uint8_t rotation) {
  if (hotspotQr.visible()) {
    drawHotspotQr();
    return;
  }
  if (nowPlayingScreen.visible) {
    drawNowPlayingScreen();
    drawOverlays();
    return;
  }
  switch (screen.content) {
    case ScreenContent::kPlaylistFrame:
    case ScreenContent::kLiveFrame:
      if (framebuffer.allocated && framebuffer.presented) {
        presentFramebuffer();
        drawOverlays();
        return;
      }
      // Both re-render through markScreen*(), which redraws the overlays.
      if (screen.content == ScreenContent::kPlaylistFrame &&
          activePlaylist.valid &&
          activePlaylist.slot == screen.slot &&
          renderPlaylistFrame(activePlaylist, screen.playlistFrame)) {
        return;
      }
      if (screen.content == ScreenContent::kLiveFrame && liveFrame.data &&
          decodeJpegBuffer(liveFrame.data, liveFrame.size, liveFrame.width,
                           liveFrame.height, liveFrame.rotation, true)) {
        drawOverlays();
        return;
      }
      break;
    case ScreenContent::kProgress:
      return;
    case ScreenContent::kFill:
      break;
  }
  display.setRotation(rotation);
  display.fillRect(x, y, width, height,
                   screen.content == ScreenContent::kFill ? screen.fillColor
                                                          : TFT_BLACK);
  drawOverlays();
}

void onScreenChanged() {
  // New media, a message, or upload progress replaced the now-playing screen
  // and the hotspot QR codes.
  nowPlayingScreen.visible = false;
  hotspotQr.stage = HotspotQrStage::kHidden;
  if (screen.content != ScreenContent::kLiveFrame) releaseLiveFrame();
  if (screen.content == ScreenContent::kProgress) {
    batteryOverlay.visible = false;
    itemHint.visible = false;
    nowPlayingOverlay.visible = false;
    nowPlayingOverlay.onButton = false;
  } else {
    drawOverlays();
  }
}

void markScreenFill(uint16_t color) {
  screen = ScreenState{};
  screen.fillColor = color;
  onScreenChanged();
}

void markScreenPlaylistFrame(uint8_t slot, uint16_t frameIndex) {
  screen = ScreenState{};
  screen.content = ScreenContent::kPlaylistFrame;
  screen.slot = slot;
  screen.playlistFrame = frameIndex;
  onScreenChanged();
}

// Called right after a live transfer was rendered; may take ownership of its
// JPEG buffer so the frame can be redrawn later.
void markScreenLiveFrame() {
  releaseLiveFrame();
  liveFrame.data = transfer.data;
  liveFrame.size = transfer.total;
  liveFrame.width = transfer.width;
  liveFrame.height = transfer.height;
  liveFrame.rotation = transfer.rotation;
  transfer.data = nullptr;
  screen = ScreenState{};
  screen.content = ScreenContent::kLiveFrame;
  onScreenChanged();
}

void markScreenProgress() {
  screen = ScreenState{};
  screen.content = ScreenContent::kProgress;
  onScreenChanged();
}

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
  markScreenProgress();
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
  markScreenFill(TFT_BLACK);
}

void waitForButtonsReleased() {
  uint32_t releasedSince = millis();
  while (millis() - releasedSince < 50) {
    for (gpio_num_t button : kButtons) {
      if (digitalRead(button) == LOW) releasedSince = millis();
    }
    delay(1);
  }
}

void enterDeepSleep() {
  waitForButtonsReleased();
  display.setBrightness(0);
  display.sleep();
  Serial.flush();
  gpio_hold_dis(static_cast<gpio_num_t>(board::kBacklight));
  pinMode(board::kBacklight, OUTPUT);
  digitalWrite(board::kBacklight, LOW);
  gpio_hold_en(static_cast<gpio_num_t>(board::kBacklight));
  {
    const gpio_num_t lcdPower = static_cast<gpio_num_t>(board::kLcdPower);
    gpio_hold_dis(lcdPower);
    pinMode(board::kLcdPower, OUTPUT);
    digitalWrite(board::kLcdPower, LOW);
    gpio_hold_en(lcdPower);
  }
  // Digital (non-RTC) pad holds only persist into deep sleep when enabled.
  gpio_deep_sleep_hold_en();

  // In deep sleep the button pads are RTC-muxed, where the digital pull-ups
  // no longer apply. Enable the RTC pull-ups on every button and keep
  // RTC_PERIPH powered so they (and the pads) are not isolated; otherwise a
  // floating pad reads low and wakes the board immediately.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  uint64_t wakeMask = 0;
  for (gpio_num_t button : kButtons) {
    rtc_gpio_init(button);
    rtc_gpio_set_direction(button, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(button);
    rtc_gpio_pullup_en(button);
    wakeMask |= 1ULL << button;
  }
  delay(5);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void showButtonMessage(const char *message) {
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_center);
  display.setTextSize(2);
  display.drawString(message, display.width() / 2, display.height() / 2);
  display.setTextDatum(top_left);
  markScreenFill(TFT_BLACK);
}

void drawHotspotQrLabel(const char *text, int16_t y, uint8_t size) {
  display.setTextSize(size);
  display.drawString(text, display.width() / 2, y);
}

void drawHotspotQr() {
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_center);
  const int16_t qrSize = display.width() - 10;
  const int16_t qrX = (display.width() - qrSize) / 2;
  const int16_t qrY = (display.height() - qrSize) / 2;
  const int16_t topLabelY = qrY - 24;
  const int16_t bottomLabelY = qrY + qrSize + 20;

  switch (hotspotQr.stage) {
    case HotspotQrStage::kWaiting:
      drawHotspotQrLabel("Hotspot on", display.height() / 2 - 12, 2);
      drawHotspotQrLabel("Starting...", display.height() / 2 + 14, 1);
      break;
    case HotspotQrStage::kJoin: {
      // Long enough for an escaped 32-byte SSID and 63-byte password.
      char payload[7 + 2 * kWifiMaxSsidLength + 9 +
                   2 * kHotspotMaxPasswordLength + 3];
      if (!wifiManager.formatHotspotJoinQr(payload, sizeof(payload))) {
        drawHotspotQrLabel("Set AP password", display.height() / 2, 2);
        break;
      }
      drawHotspotQrLabel("Scan to join", topLabelY, 2);
      display.qrcode(payload, qrX, qrY, qrSize, 1, /*margin=*/true);
      // Wipe the password from the stack once it is drawn.
      memset(payload, 0, sizeof(payload));
      drawHotspotQrLabel(wifiManager.hotspotStatus().ssid, bottomLabelY, 1);
      break;
    }
    case HotspotQrStage::kOpenPage:
      drawHotspotQrLabel("Scan to open", topLabelY, 2);
      display.qrcode(kHotspotPageUrl, qrX, qrY, qrSize, 1, /*margin=*/true);
      drawHotspotQrLabel("192.168.4.1", bottomLabelY, 1);
      break;
    case HotspotQrStage::kHidden:
      break;
  }
  display.setTextDatum(top_left);
  display.setTextSize(1);
  drawOverlays();
}

void showHotspotQr(HotspotQrStage stage) {
  if (!hotspotQr.visible()) hotspotQr.rotation = display.getRotation();
  hotspotQr.stage = stage;
  hotspotQr.stageAt = millis();
  drawHotspotQr();
}

void hideHotspotQr() {
  if (!hotspotQr.visible()) return;
  hotspotQr.stage = HotspotQrStage::kHidden;
  display.setRotation(hotspotQr.rotation);
  restoreUnderOverlay(0, 0, display.width(), display.height(),
                      hotspotQr.rotation);
}

// Turns the hotspot back off (it stops once no phone is mid-upload) and
// returns to what the screen showed before the QR codes.
void cancelHotspotFromQr() {
  wifiManager.turnOffHotspot();
  hideHotspotQr();
}

void pollHotspotQr() {
  if (!hotspotQr.visible()) return;
  const uint32_t now = millis();
  // Someone is setting up a phone: keep WiFi from being idled off.
  power.lastActivityAt = now;
  const HotspotQrStage next = nextHotspotQrStage(
      hotspotQr.stage, now - hotspotQr.stageAt,
      wifiManager.hotspotStatus().running, wifiManager.hotspotStationCount());
  if (next == hotspotQr.stage) return;
  if (next == HotspotQrStage::kHidden) {
    hideHotspotQr();
  } else {
    showHotspotQr(next);
  }
}

void showHotspotButtonFeedback(uint8_t previousMode) {
  const HotspotStatus status = wifiManager.hotspotStatus();
  if (status.mode != previousMode) {
    showButtonMessage(status.mode == HOTSPOT_OFF ? "Hotspot off" : "Hotspot on");
  } else {
    showButtonMessage("Set AP password");
  }
}

void pollButton(gpio_num_t pin, ButtonState &button) {
  const bool pressed = digitalRead(pin) == LOW;
  if (!button.armed) {
    if (!pressed) button.armed = true;
    return;
  }

  if (pressed && !button.pressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(pin) != LOW) return;
    button.pressed = true;
    button.longHandled = false;
    button.pressedAt = millis();
    return;
  }

  if (pressed && button.pressed && !button.longHandled &&
      millis() - button.pressedAt >= kHotspotButtonHoldMs) {
    if (wifiManager.suspended()) {
      // WiFi was turned off for being idle: bring it back as configured
      // rather than changing the hotspot setting.
      noteActivity();
      showButtonMessage("WiFi on");
    } else {
      const uint8_t previousMode = wifiManager.hotspotStatus().mode;
      wifiManager.toggleHotspotMode();
      const HotspotStatus status = wifiManager.hotspotStatus();
      if (status.mode != previousMode && status.mode != HOTSPOT_OFF) {
        // Turned on: show how to join (a fallback hotspot may still be
        // waiting for the home network to give up).
        noteActivity();
        showHotspotQr(status.running ? HotspotQrStage::kJoin
                                     : HotspotQrStage::kWaiting);
      } else {
        showHotspotButtonFeedback(previousMode);
      }
    }
    button.longHandled = true;
    return;
  }

  if (!pressed && button.pressed) {
    delay(kSleepButtonDebounceMs);
    if (digitalRead(pin) == LOW) return;
    const bool wasLongPress = button.longHandled;
    button.pressed = false;
    button.longHandled = false;
    if (!wasLongPress) enterDeepSleep();
  }
}

void pollButtons() {
  for (size_t i = 0; i < kButtonCount; ++i) {
    // Only one button is tracked at a time so holding both acts once.
    bool otherPressed = false;
    for (size_t j = 0; j < kButtonCount; ++j) {
      if (j != i && buttonStates[j].pressed) otherPressed = true;
    }
    if (otherPressed) continue;
    pollButton(kButtons[i], buttonStates[i]);
  }
}

void releaseTransfer() {
  free(transfer.data);
  transfer = Transfer{};
}

void framePath(char *path, size_t length, uint8_t slot,
               uint16_t frameIndex) {
  storedFramePath(path, length, slot, frameIndex);
}

void legacyFramePath(char *path, size_t length, uint8_t generation,
                     uint16_t frameIndex) {
  snprintf(path, length, "/%c%03u.jpg", generation ? 'b' : 'a', frameIndex);
}

bool removeIfExists(const char *path) {
  return !LittleFS.exists(path) || LittleFS.remove(path);
}

void storedSlotsInUse(bool used[kMaxStoredItems]) {
  for (uint8_t slot = 0; slot < kMaxStoredItems; ++slot) {
    used[slot] = storedItems[slot].used;
  }
}

// Removes the frames and both metadata files of *slot*. Frames are always
// written from 0 upwards, so the first missing one ends the frames; anything
// left past a gap by a failed removal is cleared by removeStrayStorageFiles()
// at the next boot.
bool removeSlotFiles(uint8_t slot) {
  if (!filesystemMounted) return false;
  if (framebuffer.slot == slot) framebuffer.holdsPlaylistFrame = false;
  bool removed = true;
  char path[kStoredPathSize];
  for (uint16_t i = 0; i < kMaxStoredFrames; ++i) {
    framePath(path, sizeof(path), slot, i);
    if (!LittleFS.exists(path)) break;
    if (!LittleFS.remove(path)) removed = false;
  }
  storedMetadataPath(path, sizeof(path), slot, false);
  if (!removeIfExists(path)) removed = false;
  storedMetadataPath(path, sizeof(path), slot, true);
  if (!removeIfExists(path)) removed = false;
  return removed;
}

bool cancelStaging() {
  if (!stagingPlaylist.valid) return true;
  const uint8_t rotation = stagingPlaylist.rotation;
  if (transfer.active && transfer.persistent) releaseTransfer();
  const bool removed = removeSlotFiles(stagingPlaylist.slot);
  stagingPlaylist = Playlist{};
  restoreActiveOrClear(rotation);
  return removed;
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

void allocateFramebuffer() {
  for (uint16_t *&band : framebuffer.bands) {
    band = static_cast<uint16_t *>(heap_caps_malloc(
        kFramebufferBandPixels * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!band) {
      band = static_cast<uint16_t *>(heap_caps_malloc(
          kFramebufferBandPixels * sizeof(uint16_t), MALLOC_CAP_8BIT));
    }
    if (!band) {
      // Fall back to drawing decoded blocks straight to the panel.
      for (uint16_t *&allocated : framebuffer.bands) {
        free(allocated);
        allocated = nullptr;
      }
      return;
    }
  }
  framebuffer.allocated = true;
}

uint16_t *framebufferPixel(int nativeX, int nativeY) {
  return framebuffer.bands[nativeY / kFramebufferBandRows] +
         (nativeY % kFramebufferBandRows) * board::kWidth + nativeX;
}

int jpegDraw(JPEGDRAW *draw) {
  if (jpegOutput == JpegOutput::kDisplay) {
    display.pushImage(draw->x, draw->y, draw->iWidth, draw->iHeight,
                      draw->pPixels);
  } else if (jpegOutput == JpegOutput::kFramebuffer) {
    if (draw->x >= framebuffer.width) return 1;
    const int columns =
        min<int>(draw->iWidthUsed, framebuffer.width - draw->x);
    for (int row = 0; row < draw->iHeight; ++row) {
      const int y = draw->y + row;
      if (y >= framebuffer.height) break;
      const uint16_t *source = draw->pPixels + row * draw->iWidth;
      if (framebuffer.rotation == 0) {
        memcpy(framebufferPixel(draw->x, y), source,
               columns * sizeof(uint16_t));
        continue;
      }
      for (int column = 0; column < columns; ++column) {
        const int x = draw->x + column;
        // Same mapping as LovyanGFX's rotations (Panel_FrameBufferBase).
        int nativeX, nativeY;
        switch (framebuffer.rotation) {
          case 1:
            nativeX = framebuffer.height - 1 - y;
            nativeY = x;
            break;
          case 2:
            nativeX = framebuffer.width - 1 - x;
            nativeY = framebuffer.height - 1 - y;
            break;
          default:
            nativeX = y;
            nativeY = framebuffer.width - 1 - x;
            break;
        }
        *framebufferPixel(nativeX, nativeY) = source[column];
      }
    }
  }
  return 1;
}

bool decodeJpeg(uint8_t *data, uint32_t size, uint16_t width, uint16_t height,
                uint8_t rotation, JpegOutput output) {
  if (!jpeg.openRAM(data, static_cast<int>(size), jpegDraw)) return false;
  const bool dimensionsOk =
      jpeg.getWidth() == width && jpeg.getHeight() == height;
  bool decoded = false;
  if (dimensionsOk) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    if (output == JpegOutput::kFramebuffer) {
      framebuffer.holdsPlaylistFrame = false;
      framebuffer.presented = false;
      framebuffer.rotation = rotation;
      framebuffer.width = width;
      framebuffer.height = height;
    }
    jpegOutput = output;
    if (output == JpegOutput::kDisplay) display.startWrite();
    decoded = jpeg.decode(0, 0, 0) != 0;
    if (output == JpegOutput::kDisplay) display.endWrite();
    jpegOutput = JpegOutput::kDiscard;
  }
  jpeg.close();
  return dimensionsOk && decoded;
}

void presentFramebuffer() {
  display.setRotation(0);
  display.startWrite();
  for (uint16_t y = 0, band = 0; y < board::kHeight;
       y += kFramebufferBandRows, ++band) {
    const uint16_t rows = min<int>(kFramebufferBandRows, board::kHeight - y);
    display.pushImage(0, y, board::kWidth, rows, framebuffer.bands[band]);
  }
  display.endWrite();
  // Later drawing (overlays, text) uses the frame's own orientation.
  display.setRotation(framebuffer.rotation);
  framebuffer.holdsPlaylistFrame = false;
  framebuffer.presented = true;
}

bool decodeJpegBuffer(uint8_t *data, uint32_t size, uint16_t width,
                      uint16_t height, uint8_t rotation, bool render) {
  if (!render) {
    return decodeJpeg(data, size, width, height, rotation, JpegOutput::kDiscard);
  }
  if (!framebuffer.allocated) {
    display.setRotation(rotation);
    return decodeJpeg(data, size, width, height, rotation, JpegOutput::kDisplay);
  }
  if (!decodeJpeg(data, size, width, height, rotation,
                  JpegOutput::kFramebuffer)) {
    return false;
  }
  presentFramebuffer();
  return true;
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

bool decodePlaylistFrame(const Playlist &playlist, uint16_t frameIndex,
                         bool render) {
  if (!filesystemMounted || !playlist.valid ||
      frameIndex >= playlist.frameCount) {
    return false;
  }
  char path[kStoredPathSize];
  framePath(path, sizeof(path), playlist.slot, frameIndex);
  uint8_t *data = nullptr;
  uint32_t size = 0;
  if (!readFileToBuffer(path, &data, &size)) return false;
  const uint16_t width =
      (playlist.rotation & 1) ? board::kHeight : board::kWidth;
  const uint16_t height =
      (playlist.rotation & 1) ? board::kWidth : board::kHeight;
  const bool decoded =
      render ? decodeJpegBuffer(data, size, width, height, playlist.rotation,
                                true)
             : decodeJpeg(data, size, width, height, playlist.rotation,
                          JpegOutput::kFramebuffer);
  free(data);
  return decoded;
}

// Decodes a stored frame into the framebuffer ahead of its deadline, so
// showing it later is a single push with no file read or decode delay.
void preparePlaylistFrame(const Playlist &playlist, uint16_t frameIndex) {
  if (!framebuffer.allocated ||
      (framebuffer.holdsPlaylistFrame &&
       framebuffer.slot == playlist.slot &&
       framebuffer.frameIndex == frameIndex)) {
    return;
  }
  if (!decodePlaylistFrame(playlist, frameIndex, false)) return;
  framebuffer.holdsPlaylistFrame = true;
  framebuffer.slot = playlist.slot;
  framebuffer.frameIndex = frameIndex;
}

bool renderPlaylistFrame(const Playlist &playlist, uint16_t frameIndex) {
  if (framebuffer.holdsPlaylistFrame &&
      framebuffer.slot == playlist.slot &&
      framebuffer.frameIndex == frameIndex && playlist.valid) {
    presentFramebuffer();
  } else if (!decodePlaylistFrame(playlist, frameIndex, true)) {
    return false;
  }
  markScreenPlaylistFrame(playlist.slot, frameIndex);
  return true;
}

bool playlistFilesExist(const Playlist &playlist) {
  char path[kStoredPathSize];
  for (uint16_t i = 0; i < playlist.frameCount; ++i) {
    framePath(path, sizeof(path), playlist.slot, i);
    File file = LittleFS.open(path, FILE_READ);
    if (!file || file.size() == 0 || file.size() > kMaxFrameSize) {
      if (file) file.close();
      return false;
    }
    file.close();
  }
  return true;
}

// Reads and checks /sNN.meta. Frame files are not checked here.
bool readSlotMetadata(uint8_t slot, Playlist &out) {
  out = Playlist{};
  if (!filesystemMounted || slot >= kMaxStoredItems) return false;
  char path[kStoredPathSize];
  storedMetadataPath(path, sizeof(path), slot, false);
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  const size_t size = file.size();
  if (size < storedItemMetadataSize(1) ||
      size > storedItemMetadataSize(kMaxStoredFrames)) {
    file.close();
    return false;
  }
  uint8_t metadata[storedItemMetadataSize(kMaxStoredFrames)];
  const bool read = file.read(metadata, size) == size;
  file.close();
  if (!read || !validateStoredItemMetadata(metadata, size, slot)) return false;

  Playlist loaded;
  loaded.slot = slot;
  loaded.mediaType = metadata[6];
  loaded.frameCount = metadata[7];
  loaded.storedCount = loaded.frameCount;
  loaded.rotation = metadata[8];
  loaded.loop = metadata[9] != 0;
  loaded.bytes = readLe32(metadata + 10);
  loaded.itemId = readLe32(metadata + 14);
  for (uint16_t i = 0; i < loaded.frameCount; ++i) {
    loaded.delays[i] =
        readLe16(metadata + kStoredItemMetadataHeaderSize + i * 2);
    if (loaded.delays[i] < kMinimumFrameDelayMs) return false;
  }
  loaded.valid = true;
  out = loaded;
  return true;
}

// Writes the playlist's metadata to /sNN.tmp and reads it back. The item
// only appears once commitSlotMetadata() renames it into place.
bool writeSlotMetadata(const Playlist &playlist) {
  const uint8_t frameCount = static_cast<uint8_t>(playlist.frameCount);
  const size_t size = storedItemMetadataSize(frameCount);
  uint8_t metadata[storedItemMetadataSize(kMaxStoredFrames)]{};
  metadata[0] = 'C';
  metadata[1] = 'C';
  metadata[2] = 'M';
  metadata[3] = 'I';
  metadata[4] = kStoredItemMetadataVersion;
  metadata[5] = playlist.slot;
  metadata[6] = playlist.mediaType;
  metadata[7] = frameCount;
  metadata[8] = playlist.rotation;
  metadata[9] = playlist.loop ? 1 : 0;
  writeLe32(metadata + 10, playlist.bytes);
  writeLe32(metadata + 14, playlist.itemId);
  for (uint16_t i = 0; i < frameCount; ++i) {
    writeLe16(metadata + kStoredItemMetadataHeaderSize + i * 2,
              playlist.delays[i]);
  }
  writeLe16(metadata + size - kPlaylistMetadataCrcSize,
            crc16CcittFalse(metadata, size - kPlaylistMetadataCrcSize));

  char path[kStoredPathSize];
  storedMetadataPath(path, sizeof(path), playlist.slot, true);
  LittleFS.remove(path);
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) return false;
  const bool written = file.write(metadata, size) == size;
  file.flush();
  file.close();
  if (!written) {
    LittleFS.remove(path);
    return false;
  }

  File verify = LittleFS.open(path, FILE_READ);
  uint8_t check[storedItemMetadataSize(kMaxStoredFrames)];
  const bool verified =
      verify && verify.size() == size && verify.read(check, size) == size &&
      validateStoredItemMetadata(check, size, playlist.slot) &&
      memcmp(metadata, check, size) == 0;
  if (verify) verify.close();
  if (!verified) {
    LittleFS.remove(path);
    return false;
  }
  return true;
}

bool commitSlotMetadata(uint8_t slot) {
  char temporary[kStoredPathSize];
  char finalPath[kStoredPathSize];
  storedMetadataPath(temporary, sizeof(temporary), slot, true);
  storedMetadataPath(finalPath, sizeof(finalPath), slot, false);
  return removeIfExists(finalPath) && LittleFS.rename(temporary, finalPath);
}

void rememberStoredItem(const Playlist &playlist) {
  StoredItem &item = storedItems[playlist.slot];
  item.bytes = playlist.bytes;
  item.itemId = playlist.itemId;
  item.frameCount = playlist.frameCount;
  item.mediaType = playlist.mediaType;
  item.rotation = playlist.rotation;
  item.loop = playlist.loop;
  item.used = true;
}

// /active holds the slot and its complement, so a torn write reads as none.
void writeActiveSlot(uint8_t slot) {
  activeSlot = slot;
  if (!filesystemMounted) return;
  File file = LittleFS.open(kActiveSlotPath, FILE_WRITE);
  if (!file) return;
  const uint8_t data[2] = {slot, static_cast<uint8_t>(~slot)};
  file.write(data, sizeof(data));
  file.close();
}

uint8_t readActiveSlot() {
  File file = LittleFS.open(kActiveSlotPath, FILE_READ);
  if (!file) return kNoStoredSlot;
  uint8_t data[2] = {};
  const bool read = file.read(data, sizeof(data)) == sizeof(data);
  file.close();
  if (!read || data[1] != static_cast<uint8_t>(~data[0]) ||
      data[0] >= kMaxStoredItems) {
    return kNoStoredSlot;
  }
  return data[0];
}

void loadStoredLibrary() {
  for (uint8_t slot = 0; slot < kMaxStoredItems; ++slot) {
    storedItems[slot] = StoredItem{};
    Playlist item;
    if (readSlotMetadata(slot, item)) rememberStoredItem(item);
  }
  activeSlot = readActiveSlot();
  if (activeSlot == kNoStoredSlot || !storedItems[activeSlot].used) {
    bool used[kMaxStoredItems];
    storedSlotsInUse(used);
    activeSlot = firstUsedStoredSlot(used);
  }
}

// Moves the single item kept by firmware before the library into a slot.
void migrateLegacyMedia() {
  File file = LittleFS.open(kLegacyMetadataPath, FILE_READ);
  if (!file) return;
  const size_t size = file.size();
  uint8_t metadata[playlistMetadataSize(kMaxStoredFrames)];
  const bool read = size >= playlistMetadataSize(1) &&
                    size <= sizeof(metadata) &&
                    file.read(metadata, size) == size;
  file.close();

  if (read && validatePlaylistMetadata(metadata, size)) {
    Playlist item;
    item.slot = 0;
    item.mediaType = metadata[6];
    item.frameCount = metadata[7];
    item.storedCount = item.frameCount;
    item.rotation = metadata[8];
    item.loop = metadata[9] != 0;
    item.itemId = esp_random();
    const uint8_t generation = metadata[5];
    bool moved = removeSlotFiles(item.slot);
    char from[kStoredPathSize];
    char to[kStoredPathSize];
    for (uint16_t i = 0; moved && i < item.frameCount; ++i) {
      item.delays[i] = max(
          readLe16(metadata + kPlaylistMetadataHeaderSize + i * 2),
          kMinimumFrameDelayMs);
      legacyFramePath(from, sizeof(from), generation, i);
      framePath(to, sizeof(to), item.slot, i);
      File frame = LittleFS.open(from, FILE_READ);
      if (!frame) {
        moved = false;
        break;
      }
      item.bytes += frame.size();
      frame.close();
      moved = LittleFS.rename(from, to);
    }
    if (moved && writeSlotMetadata(item) && commitSlotMetadata(item.slot)) {
      writeActiveSlot(item.slot);
    } else {
      removeSlotFiles(item.slot);
    }
  }
  LittleFS.remove(kLegacyMetadataPath);
  removeIfExists(kLegacyMetadataTempPath);
}

// Removes files no saved item owns: frames of an upload that was cut off by
// a reset, unfinished metadata, and leftover frames from the old layout.
void removeStrayStorageFiles() {
  constexpr size_t kBatch = 32;
  for (uint8_t pass = 0; pass < 16; ++pass) {
    char doomed[kBatch][kStoredPathSize + 1];
    size_t count = 0;
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) return;
    for (File entry = root.openNextFile(); entry && count < kBatch;
         entry = root.openNextFile()) {
      const char *name = entry.name();
      if (name && *name == '/') ++name;
      const uint8_t slot = storedFileSlot(name);
      const bool stray =
          isLegacyFrameName(name) ||
          (slot != kNoStoredSlot &&
           (!storedItems[slot].used || strstr(name, ".tmp") != nullptr));
      if (stray && strlen(name) < kStoredPathSize) {
        snprintf(doomed[count++], sizeof(doomed[0]), "/%s", name);
      }
      entry.close();
    }
    root.close();
    if (count == 0) return;
    for (size_t i = 0; i < count; ++i) LittleFS.remove(doomed[i]);
  }
}

bool loadActiveMetadata() {
  activePlaylist = Playlist{};
  if (!filesystemMounted || activeSlot >= kMaxStoredItems ||
      !storedItems[activeSlot].used) {
    return false;
  }
  Playlist loaded;
  if (!readSlotMetadata(activeSlot, loaded) || !playlistFilesExist(loaded)) {
    return false;
  }
  activePlaylist = loaded;
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
  // Paused under the now-playing screen. Afterwards the loop below skips
  // ahead to the frame that is due, as after any other late wake-up.
  if (!playbackActive()) return;
  if (static_cast<int32_t>(millis() - playback.deadline) < 0) {
    preparePlaylistFrame(activePlaylist, playback.nextFrame);
    return;
  }

  uint32_t now = millis();
  // A GIF that cannot keep its frame times at the reduced clock finishes
  // this playback at full speed.
  if (!power.fullSpeed) {
    playback.lateFrames =
        updateLateFrameStreak(playback.lateFrames, now - playback.deadline);
    if (playback.lateFrames >= kLateFramesBeforeFullSpeed) {
      playback.needsFullSpeed = true;
    }
  }
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
  // STATUS_RESPONSE: transferActive u8, backlight u8, transferId u16,
  // received u32, expected u32, freeHeap u32, then diagnostics read by
  // scripts/device.py: batteryMillivolts u16, onBattery u8, cpuMhz u16,
  // uptimeMs u32 (all little-endian). Readers ignore bytes they don't know.
  uint8_t payload[25] = {static_cast<uint8_t>(transfer.active), backlight};
  writeLe16(payload + 2, transfer.active ? transfer.id : 0);
  writeLe32(payload + 4, transfer.received);
  writeLe32(payload + 8, transfer.total);
  writeLe32(payload + 12, ESP.getFreeHeap());
  writeLe16(payload + 16, static_cast<uint16_t>(readBatteryMillivolts()));
  payload[18] = power.onBattery ? 1 : 0;
  writeLe16(payload + 19, static_cast<uint16_t>(getCpuFrequencyMhz()));
  writeLe32(payload + 21, millis());
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
  char path[kStoredPathSize];
  framePath(path, sizeof(path), stagingPlaylist.slot,
            transfer.frameIndex);
  framebuffer.holdsPlaylistFrame = false;
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

// Deletes every saved item, including an upload in progress.
bool clearStoredMedia() {
  playback = Playback{};
  releaseTransfer();
  stagingPlaylist = Playlist{};
  uploadProgress = UploadProgress{};
  if (!filesystemMounted) return false;
  activePlaylist = Playlist{};
  bool removed = true;
  for (uint8_t slot = 0; slot < kMaxStoredItems; ++slot) {
    if (removeSlotFiles(slot)) storedItems[slot] = StoredItem{};
    else removed = false;
  }
  removeStrayStorageFiles();
  if (!removeIfExists(kActiveSlotPath)) removed = false;
  activeSlot = kNoStoredSlot;
  if (removed) return true;
  loadStoredLibrary();
  if (loadActiveMetadata()) startStoredPlayback();
  return false;
}

// Shows a saved item and remembers it as the one to show after a restart.
// The previous item stays on screen if this one cannot be read.
bool selectStoredSlot(uint8_t slot) {
  if (slot >= kMaxStoredItems || !storedItems[slot].used) return false;
  Playlist loaded;
  if (!readSlotMetadata(slot, loaded) || !playlistFilesExist(loaded)) {
    return false;
  }
  const Playlist previous = activePlaylist;
  activePlaylist = loaded;
  if (!startStoredPlayback()) {
    activePlaylist = previous;
    if (activePlaylist.valid) startStoredPlayback();
    return false;
  }
  writeActiveSlot(slot);
  return true;
}

// Tap on the second button: show the next saved item, wrapping
// around, with a short "2 of 3" hint. With one item it is shown again (for
// example after a live frame replaced it).
void showNextStoredItem() {
  // A battery indicator still up from an earlier hold is replaced along
  // with the whole screen, so it just goes away.
  const bool batteryWasVisible = batteryOverlay.visible;
  batteryOverlay.visible = false;
  if (transfer.active || stagingPlaylist.valid) {
    if (batteryWasVisible) {
      batteryOverlay.visible = true;
      hideBatteryOverlay();
    }
    return;
  }
  bool used[kMaxStoredItems];
  storedSlotsInUse(used);
  const bool activeUsed = activeSlot < kMaxStoredItems && used[activeSlot];
  uint8_t next = activeUsed ? nextStoredSlot(used, activeSlot)
                            : firstUsedStoredSlot(used);
  if (next == kNoStoredSlot && activeUsed) next = activeSlot;

  // Skip items that cannot be read rather than stopping on them.
  while (next != kNoStoredSlot && !selectStoredSlot(next)) {
    used[next] = false;
    next = nextStoredSlot(used, next);
  }
  if (next == kNoStoredSlot) {
    if (batteryWasVisible) {
      batteryOverlay.visible = true;
      hideBatteryOverlay();
    }
    showItemHint("No saved media");
    return;
  }
  storedSlotsInUse(used);
  uint8_t position = 0;
  uint8_t count = 0;
  storedItemPosition(used, next, &position, &count);
  char text[sizeof(itemHint.text)];
  snprintf(text, sizeof(text), "%u of %u", static_cast<unsigned>(position),
           static_cast<unsigned>(count));
  showItemHint(text);
}

// Deletes one saved item. Deleting the item on screen moves on to the next
// saved item, or clears the screen when it was the last one.
bool deleteStoredSlot(uint8_t slot) {
  const bool wasActive = slot == activeSlot;
  const uint8_t rotation = activePlaylist.rotation;
  if (wasActive) {
    playback = Playback{};
    activePlaylist = Playlist{};
  }
  const bool removed = removeSlotFiles(slot);
  if (removed) storedItems[slot] = StoredItem{};
  if (!wasActive) return removed;

  bool used[kMaxStoredItems];
  storedSlotsInUse(used);
  uint8_t next = nextStoredSlot(used, slot);
  while (next != kNoStoredSlot && !selectStoredSlot(next)) {
    used[next] = false;
    next = nextStoredSlot(used, slot);
  }
  if (next == kNoStoredSlot) {
    writeActiveSlot(kNoStoredSlot);
    display.setRotation(rotation);
    display.fillScreen(TFT_BLACK);
    markScreenFill(TFT_BLACK);
  }
  return removed;
}

void sendStoredList(uint16_t sequence) {
  uint8_t payload[kStoredListHeaderSize +
                  kMaxStoredItems * kStoredListEntrySize];
  writeLe32(payload, static_cast<uint32_t>(LittleFS.totalBytes()));
  writeLe32(payload + 4, static_cast<uint32_t>(LittleFS.usedBytes()));
  payload[8] = kMaxStoredItems;
  payload[9] = activeSlot < kMaxStoredItems && storedItems[activeSlot].used
                   ? activeSlot
                   : kNoStoredSlot;
  uint8_t count = 0;
  size_t offset = kStoredListHeaderSize;
  for (uint8_t slot = 0; slot < kMaxStoredItems; ++slot) {
    const StoredItem &item = storedItems[slot];
    if (!item.used) continue;
    payload[offset] = slot;
    payload[offset + 1] = item.mediaType;
    writeLe16(payload + offset + 2, item.frameCount);
    payload[offset + 4] = item.rotation;
    payload[offset + 5] = item.loop ? 1 : 0;
    writeLe32(payload + offset + 6, item.bytes);
    writeLe32(payload + offset + 10, item.itemId);
    offset += kStoredListEntrySize;
    ++count;
  }
  payload[10] = count;
  writeFrame(STORED_LIST_RESPONSE, sequence, payload, offset);
}

void sendStoredFrame(uint16_t sequence, const uint8_t *request) {
  const uint8_t slot = request[0];
  const uint16_t frameIndex = readLe16(request + 1);
  const uint32_t offset = readLe32(request + 3);
  const uint16_t maxLength = readLe16(request + 7);
  if (slot >= kMaxStoredItems || !storedItems[slot].used ||
      frameIndex >= storedItems[slot].frameCount || maxLength == 0) {
    sendNack(sequence, READ_STORED_FRAME, OUT_OF_RANGE);
    return;
  }
  char path[kStoredPathSize];
  framePath(path, sizeof(path), slot, frameIndex);
  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    sendNack(sequence, READ_STORED_FRAME, OUT_OF_RANGE);
    return;
  }
  const uint32_t size = static_cast<uint32_t>(file.size());
  if (offset > size) {
    file.close();
    sendNack(sequence, READ_STORED_FRAME, OUT_OF_RANGE);
    return;
  }
  uint32_t length = size - offset;
  if (length > maxLength) length = maxLength;
  if (length > kMaxStoredFrameReadData) length = kMaxStoredFrameReadData;

  static uint8_t payload[kStoredFrameResponseHeaderSize +
                         kMaxStoredFrameReadData];
  payload[0] = slot;
  writeLe16(payload + 1, frameIndex);
  writeLe32(payload + 3, size);
  writeLe32(payload + 7, offset);
  const bool read = file.seek(offset) &&
                    file.read(payload + kStoredFrameResponseHeaderSize,
                              length) == length;
  file.close();
  if (!read) {
    sendNack(sequence, READ_STORED_FRAME, NO_MEMORY);
    return;
  }
  writeFrame(STORED_FRAME_RESPONSE, sequence, payload,
             kStoredFrameResponseHeaderSize + length);
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
        // Almost always a full flash.
        sendNack(sequence, command, STORAGE_FULL);
        releaseTransfer();
        return;
      }
      if (transfer.persistent) {
        stagingPlaylist.delays[transfer.frameIndex] =
            max(transfer.delayMs, kMinimumFrameDelayMs);
        stagingPlaylist.bytes += transfer.total;
        ++stagingPlaylist.storedCount;
        updateUploadProgress(stagingPlaylist.storedCount, 0, 0);
      } else if (!decodeTransfer(true)) {
        sendNack(sequence, command, DECODE_FAILED);
        releaseTransfer();
        return;
      } else {
        markScreenLiveFrame();
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
        applyBacklight();
        sendAck(sequence, command);
      }
      return;

    case CLEAR_DISPLAY:
      if (length != 0 && length != 2) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        const uint16_t color = length ? readLe16(payload) : 0;
        display.fillScreen(color);
        markScreenFill(color);
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
      // A new upload goes into a free slot; saved items are never replaced.
      bool used[kMaxStoredItems];
      storedSlotsInUse(used);
      const uint8_t slot = firstFreeStoredSlot(used);
      if (slot == kNoStoredSlot) {
        sendNack(sequence, command, STORAGE_FULL);
        return;
      }
      if (!removeSlotFiles(slot)) {
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      stagingPlaylist = Playlist{};
      stagingPlaylist.mediaType = mediaType;
      stagingPlaylist.frameCount = frameCount;
      stagingPlaylist.rotation = rotation;
      stagingPlaylist.slot = slot;
      stagingPlaylist.itemId = esp_random();
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
      if (!writeSlotMetadata(stagingPlaylist)) {
        sendNack(sequence, command, STORAGE_FULL);
        return;
      }
      const Playlist previous = activePlaylist;
      char temporary[kStoredPathSize];
      storedMetadataPath(temporary, sizeof(temporary), stagingPlaylist.slot,
                         true);
      if (!renderPlaylistFrame(stagingPlaylist, 0)) {
        LittleFS.remove(temporary);
        sendNack(sequence, command, DECODE_FAILED);
        return;
      }
      if (!commitSlotMetadata(stagingPlaylist.slot)) {
        LittleFS.remove(temporary);
        activePlaylist = previous;
        restoreActiveOrClear(stagingPlaylist.rotation);
        sendNack(sequence, command, NO_MEMORY);
        return;
      }
      activePlaylist = stagingPlaylist;
      activePlaylist.valid = true;
      rememberStoredItem(activePlaylist);
      writeActiveSlot(activePlaylist.slot);
      stagingPlaylist = Playlist{};
      uploadProgress = UploadProgress{};
      schedulePlaybackAfterFirstFrame();
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

    case LIST_STORED:
      if (length != 0) sendNack(sequence, command, INVALID_PAYLOAD);
      else if (!filesystemMounted) sendNack(sequence, command, NO_MEMORY);
      else sendStoredList(sequence);
      return;

    case SELECT_STORED:
    case DELETE_STORED: {
      if (length != 1) {
        sendNack(sequence, command, INVALID_PAYLOAD);
        return;
      }
      if (transfer.active || stagingPlaylist.valid) {
        sendNack(sequence, command, cyberclip::BUSY);
        return;
      }
      const uint8_t slot = payload[0];
      if (slot >= kMaxStoredItems || !storedItems[slot].used) {
        sendNack(sequence, command, OUT_OF_RANGE);
        return;
      }
      const bool done = command == SELECT_STORED ? selectStoredSlot(slot)
                                                 : deleteStoredSlot(slot);
      if (done) sendAck(sequence, command);
      else if (command == SELECT_STORED) sendNack(sequence, command, DECODE_FAILED);
      else sendNack(sequence, command, NO_MEMORY);
      return;
    }

    case READ_STORED_FRAME:
      if (length != kReadStoredFramePayloadSize) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else if (!filesystemMounted) {
        sendNack(sequence, command, NO_MEMORY);
      } else {
        sendStoredFrame(sequence, payload);
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

    case TURN_OFF_HOTSPOT:
      if (length != 0) {
        sendNack(sequence, command, INVALID_PAYLOAD);
      } else {
        // Reply first: this may be the hotspot page's own connection.
        sendAck(sequence, command);
        cancelHotspotFromQr();
      }
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

// Survives software and watchdog resets (see boot_guard.h).
RTC_NOINIT_ATTR BootGuardState bootGuard;
BootAction bootAction = BootAction::kNormal;
bool bootMarkedStable = false;

void showSafeModeMessage() {
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(middle_center);
  display.setTextSize(2);
  const int32_t x = display.width() / 2;
  const int32_t y = display.height() / 2;
  display.drawString("Safe mode", x, y - 24);
  display.setTextSize(1);
  display.drawString("Reinstall firmware", x, y + 4);
  display.drawString("over USB", x, y + 18);
  display.setTextDatum(top_left);
  markScreenFill(TFT_BLACK);
}

void setup() {
  // First, before anything that could be what keeps crashing.
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_PANIC ||
                         reason == ESP_RST_INT_WDT ||
                         reason == ESP_RST_TASK_WDT;
    bootAction = bootGuardOnBoot(bootGuard, crashed);
    if (bootAction == BootAction::kDownloadMode) {
      // Same as the installer's 1200-baud reboot: the ROM stays in download
      // mode, where the web installer can flash the board.
      REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
      esp_restart();
    }
  }
  setCpuFrequencyMhz(kCpuFrequencyMhz);
  Serial.setRxBufferSize(8192);
  Serial.begin(kSerialBaud);
  for (gpio_num_t button : kButtons) {
    rtc_gpio_deinit(button);
    pinMode(button, INPUT_PULLUP);
  }
  gpio_deep_sleep_hold_dis();
  pinMode(board::kBatteryButton, INPUT_PULLUP);
  analogSetPinAttenuation(board::kBatteryAdc, ADC_11db);
  gpio_hold_dis(static_cast<gpio_num_t>(board::kBacklight));
  gpio_hold_dis(static_cast<gpio_num_t>(board::kLcdPower));
  pinMode(board::kLcdPower, OUTPUT);
  digitalWrite(board::kLcdPower, HIGH);
  display.init();
  applyBacklight();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  // Before WiFi starts, while the heap is least fragmented.
  allocateFramebuffer();

  if (bootAction == BootAction::kSafeMode) {
    // Saved media and WiFi are the likely crash causes; leave them off and
    // keep USB up so the firmware can be reinstalled.
    showSafeModeMessage();
#if !CYBERCLIP_BLE_MEDIA
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
    return;
  }

  filesystemMounted = LittleFS.begin(false);
  if (!filesystemMounted) filesystemMounted = LittleFS.begin(true);
  if (filesystemMounted) {
    migrateLegacyMedia();
    loadStoredLibrary();
    removeStrayStorageFiles();
    if (loadActiveMetadata()) startStoredPlayback();
  }

  wifiManager.begin();
#if CYBERCLIP_BLE_MEDIA
  bleMedia.begin();
#else
  // Bluetooth is compiled out; hand the controller's reserved RAM back.
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
}

void loop() {
  bool busy = false;
  currentReplyTarget = ReplyTarget::kSerial;
  if (Serial.available() > 0) {
    busy = true;
    noteActivity();
  }
  while (Serial.available() > 0) {
    serialParser.feed(static_cast<uint8_t>(Serial.read()));
  }
  serialParser.pollTimeout();

  if (bootAction == BootAction::kSafeMode) {
    // USB only; the installer's 1200-baud reboot works from here.
    delay(10);
    return;
  }
  if (!bootMarkedStable && millis() >= kBootStableMs) {
    bootGuardMarkStable(bootGuard);
    bootMarkedStable = true;
  }

  currentReplyTarget = ReplyTarget::kWebSocket;
  uint8_t wsByte;
  while (wsReadByte(&wsByte)) {
    if (!busy) {
      busy = true;
      noteActivity();
    }
    wsParser.feed(wsByte);
  }
  wsParser.pollTimeout();

  wifiManager.poll();
  wsServerPoll();
  advanceStoredPlayback();
  pollButtons();
  pollBatteryButton();
  pollBatteryOverlay();
  pollItemHint();
  pollNowPlaying();
  pollHotspotQr();
  updatePowerMode();

  // Sleeping lets the idle task halt the CPU between passes instead of
  // spinning. Drawing has already finished, so this never splits a frame.
  // On battery with the clock reduced (static screen, nothing arriving), the
  // loop wakes far less often; activity switches back to 1 ms passes.
  const uint32_t sleepMs = loopIdleSleepMs(
      busy || transfer.active, playbackActive(),
      static_cast<int32_t>(playback.deadline - millis()),
      power.fullSpeed ? kLoopIdleSleepMs : kBatteryIdleSleepMs);
  if (sleepMs > 0) delay(sleepMs);
  else yield();
}
