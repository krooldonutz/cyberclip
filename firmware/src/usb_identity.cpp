// USB identity for boards that run the native USB port through TinyUSB.
//
// The Arduino core hard-codes generic "TinyUSB CDC"/"TinyUSB Device" strings
// and uses the variant's PID (0x1001, the same as the chip's built-in
// USB-Serial/JTAG unit). Overriding the weak TinyUSB descriptor callbacks lets
// hosts such as the browser serial-port picker show the product name instead.
#include <sdkconfig.h>

#if ARDUINO_USB_MODE || !CONFIG_TINYUSB_ENABLED
#error "The T-Display-S3 build requires TinyUSB (ARDUINO_USB_MODE=0)."
#endif

#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-tinyusb.h"

namespace {
constexpr char kUsbName[] = "Pixie Pixel Gear";
constexpr uint16_t kUsbVendorId = 0x303A;
// Arduino-ESP32's default TinyUSB PID; distinct from USB-Serial/JTAG (0x1001)
// so hosts do not reuse the cached name of the download-mode device.
constexpr uint16_t kUsbProductId = 0x0002;

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDOINT0_SIZE,
    .idVendor = kUsbVendorId,
    .idProduct = kUsbProductId,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const char *serialNumber() {
  static char serial[13] = {};
  if (!serial[0]) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  return serial;
}
}  // namespace

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
  return reinterpret_cast<uint8_t const *>(&kDeviceDescriptor);
}

// Index 0 is the language, 3 the serial number; every other string
// (manufacturer, product, CDC interface, configuration) uses the product name.
extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index,
                                                     uint16_t /*langid*/) {
  static uint16_t descriptor[1 + 32];
  size_t length = 0;
  if (index == 0) {
    descriptor[1] = 0x0409;
    length = 1;
  } else {
    const char *text = index == 3 ? serialNumber() : kUsbName;
    length = strlen(text);
    if (length > 32) length = 32;
    for (size_t i = 0; i < length; ++i) descriptor[1 + i] = text[i];
  }
  descriptor[0] = static_cast<uint16_t>((TUSB_DESC_STRING << 8) |
                                        (2 * length + 2));
  return descriptor;
}
