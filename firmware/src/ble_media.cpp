#include "ble_media.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace cyberclip {

BleMedia bleMedia;

namespace {

constexpr char kBleName[] = "CyberClip";
// 760 ms, one of the advertising intervals Apple recommends for accessories.
// The iPhone reconnects within a few seconds; a shorter interval only costs
// battery.
constexpr uint16_t kAdvertisingInterval = 1216;  // 0.625 ms units
constexpr uint32_t kSetupTaskStack = 4096;
constexpr UBaseType_t kSetupTaskPriority = 1;

portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override {
    bleMedia.onConnected(connInfo.getConnHandle());
    // AMS characteristics need an encrypted link. This pairs a new iPhone
    // (iOS shows a "Pair" prompt) or re-encrypts with a bonded one.
    NimBLEDevice::startSecurity(connInfo.getConnHandle());
  }

  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleMedia.onDisconnected();
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    if (connInfo.isEncrypted()) {
      bleMedia.onEncrypted(connInfo.getConnHandle());
    } else {
      // Pairing was declined or failed. Only one connection is allowed, so
      // drop it to start advertising again.
      NimBLEDevice::getServer()->disconnect(connInfo);
    }
  }
} serverCallbacks;

void setupTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bleMedia.subscribePendingConnection();
  }
}

void onAmsNotification(NimBLERemoteCharacteristic *, uint8_t *data,
                       size_t length, bool) {
  bleMedia.onEntityUpdate(data, length);
}

void startAdvertising() {
  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  // Soliciting AMS makes iOS list Cyberclip under Settings > Bluetooth so it
  // can be paired without an app.
  uint8_t solicitation[2 + sizeof(ams::kServiceUuidLe)] = {
      static_cast<uint8_t>(1 + sizeof(ams::kServiceUuidLe)),
      BLE_HS_ADV_TYPE_SOL_UUIDS128};
  memcpy(solicitation + 2, ams::kServiceUuidLe, sizeof(ams::kServiceUuidLe));
  advertisement.addData(solicitation, sizeof(solicitation));

  NimBLEAdvertisementData scanResponse;
  scanResponse.setName(kBleName);

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advertisement);
  advertising->setScanResponseData(scanResponse);
  advertising->setMinInterval(kAdvertisingInterval);
  advertising->setMaxInterval(kAdvertisingInterval);
  advertising->start();
}

}  // namespace

void BleMedia::begin() {
  NimBLEDevice::init(kBleName);
  // The board has no display input or keypad, so pairing uses Just Works
  // with bonding: the iPhone remembers Cyberclip and reconnects on its own.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  TaskHandle_t task = nullptr;
  xTaskCreate(setupTask, "ams_setup", kSetupTaskStack, nullptr,
              kSetupTaskPriority, &task);
  setupTask_ = task;

  startAdvertising();
}

uint32_t BleMedia::revision() const {
  portENTER_CRITICAL(&stateLock);
  const uint32_t revision = state_.revision;
  portEXIT_CRITICAL(&stateLock);
  return revision;
}

NowPlaying BleMedia::snapshot() const {
  portENTER_CRITICAL(&stateLock);
  const NowPlaying copy = state_;
  portEXIT_CRITICAL(&stateLock);
  return copy;
}

void BleMedia::markChanged() { ++state_.revision; }

void BleMedia::onConnected(uint16_t connHandle) {
  portENTER_CRITICAL(&stateLock);
  pendingConnHandle_ = connHandle;
  portEXIT_CRITICAL(&stateLock);
}

void BleMedia::onEncrypted(uint16_t connHandle) {
  portENTER_CRITICAL(&stateLock);
  pendingConnHandle_ = connHandle;
  portEXIT_CRITICAL(&stateLock);
  // GATT discovery blocks until the iPhone answers, which would deadlock
  // inside the host task's callback, so it runs on its own task.
  if (setupTask_) xTaskNotifyGive(static_cast<TaskHandle_t>(setupTask_));
}

void BleMedia::onDisconnected() {
  portENTER_CRITICAL(&stateLock);
  pendingConnHandle_ = 0xffff;
  const uint32_t revision = state_.revision;
  state_ = NowPlaying{};
  state_.revision = revision;
  markChanged();
  portEXIT_CRITICAL(&stateLock);
}

void BleMedia::subscribePendingConnection() {
  portENTER_CRITICAL(&stateLock);
  const uint16_t connHandle = pendingConnHandle_;
  portEXIT_CRITICAL(&stateLock);
  if (connHandle == 0xffff) return;

  NimBLEServer *server = NimBLEDevice::getServer();
  NimBLEClient *client = server ? server->getClient(connHandle) : nullptr;
  // Not an iPhone, or iOS has not exposed AMS on this link.
  NimBLERemoteService *service =
      client ? client->getService(ams::kServiceUuid) : nullptr;
  NimBLERemoteCharacteristic *entityUpdate =
      service ? service->getCharacteristic(ams::kEntityUpdateUuid) : nullptr;
  if (!entityUpdate || !entityUpdate->subscribe(true, onAmsNotification) ||
      !entityUpdate->writeValue(ams::kTrackSubscription,
                                sizeof(ams::kTrackSubscription), true) ||
      !entityUpdate->writeValue(ams::kPlayerSubscription,
                                sizeof(ams::kPlayerSubscription), true)) {
    return;
  }

  portENTER_CRITICAL(&stateLock);
  if (pendingConnHandle_ == connHandle && !state_.connected) {
    state_.connected = true;
    markChanged();
  }
  portEXIT_CRITICAL(&stateLock);
}

void BleMedia::onEntityUpdate(const uint8_t *data, size_t length) {
  ams::EntityUpdate update;
  if (!ams::parseEntityUpdate(data, length, &update)) return;

  portENTER_CRITICAL(&stateLock);
  bool changed = false;
  if (update.entity == ams::ENTITY_TRACK) {
    if (update.attribute == ams::TRACK_TITLE) {
      changed = ams::storeUtf8(state_.title, sizeof(state_.title),
                               update.value, update.valueLength);
    } else if (update.attribute == ams::TRACK_ARTIST) {
      changed = ams::storeUtf8(state_.artist, sizeof(state_.artist),
                               update.value, update.valueLength);
    }
  } else if (update.entity == ams::ENTITY_PLAYER &&
             update.attribute == ams::PLAYER_PLAYBACK_INFO) {
    const uint8_t playback =
        ams::parsePlaybackState(update.value, update.valueLength);
    if (playback != ams::PLAYBACK_UNKNOWN && playback != state_.playback) {
      state_.playback = playback;
      changed = true;
    }
  }
  if (changed) markChanged();
  portEXIT_CRITICAL(&stateLock);
}

}  // namespace cyberclip
