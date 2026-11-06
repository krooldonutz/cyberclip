// USB identity of the ESP32-S3's built-in USB-Serial/JTAG unit, which the
// board shows up as in ROM download mode. Kept apart from flasher.js so pages
// that only need to recognise it do not load esptool.
export const ESPRESSIF_USB_VENDOR_ID = 0x303a;
export const USB_JTAG_SERIAL_PRODUCT_ID = 0x1001;

export function isDownloadPort(port) {
  const info = port?.getInfo?.() ?? {};
  return info.usbVendorId === ESPRESSIF_USB_VENDOR_ID
    && info.usbProductId === USB_JTAG_SERIAL_PRODUCT_ID;
}
