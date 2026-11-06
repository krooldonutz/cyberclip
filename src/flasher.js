import { ESPLoader, Transport } from 'esptool-js';
import {
  ESPRESSIF_USB_VENDOR_ID,
  USB_JTAG_SERIAL_PRODUCT_ID,
  isDownloadPort,
} from './downloadPort.js';

export { ESPRESSIF_USB_VENDOR_ID, USB_JTAG_SERIAL_PRODUCT_ID, isDownloadPort };

export const FIRMWARE_MANIFEST_URL = '/firmware/manifest.json';

// Fallback reset after flashing. The ESP32-S3's native USB-Serial/JTAG unit
// needs a longer reset pulse than a USB-UART bridge would.
const RESET_SEQUENCE = 'D0|R1|W200|R0|W500';

export const ESP32S3_RTC_CNTL_OPTION1_REG = 0x6000812c;
export const ESP32S3_RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1;
// RTC watchdog registers, used to restart the chip without the RST button or
// relying on the host toggling RTS (same sequence as esptool's
// watchdog_reset for the ESP32-S3).
export const ESP32S3_RTC_CNTL_WDTCONFIG0_REG = 0x60008098;
export const ESP32S3_RTC_CNTL_WDTCONFIG1_REG = 0x6000809c;
export const ESP32S3_RTC_CNTL_WDTWPROTECT_REG = 0x600080b0;
export const ESP32S3_RTC_CNTL_WDT_WKEY = 0x50d83aa1;
export const DOWNLOAD_PORT_FILTER = Object.freeze({
  usbVendorId: ESPRESSIF_USB_VENDOR_ID,
  usbProductId: USB_JTAG_SERIAL_PRODUCT_ID,
});
export const DOWNLOAD_PORT_NOT_AUTHORIZED_MESSAGE =
  'The board restarted into download mode. Select Install firmware again and choose "USB JTAG/serial debug unit".';
export const INSTALL_INTERRUPTED_MESSAGE =
  'Installation was interrupted. The board is waiting in update mode; select Install firmware again to finish.';
// How long to wait for an already-authorized download port before asking the
// user to pick it (the click that started the install may still be active).
const AUTHORIZED_PORT_WAIT_MS = 2500;
const DOWNLOAD_PORT_TIMEOUT_MS = 10000;
const DOWNLOAD_PORT_POLL_MS = 250;
const BOOTLOADER_ATTEMPTS = 3;

// ESP-IDF partition table: 32-byte entries at 0x8000, starting with 0xAA 0x50.
const PARTITION_TABLE_OFFSET = 0x8000;
const PARTITION_TABLE_SIZE = 0xc00;
const PARTITION_ENTRY_SIZE = 32;
const PARTITION_TYPE_DATA = 0x01;
const PARTITION_SUBTYPE_NVS = 0x02;
const PARTITION_SUBTYPE_NVS_KEYS = 0x04;

const sleep = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });

export class DownloadPortNotAuthorizedError extends Error {
  constructor() {
    super(DOWNLOAD_PORT_NOT_AUTHORIZED_MESSAGE);
    this.name = 'DownloadPortNotAuthorizedError';
  }
}

// Thrown when the board was left in download mode (flash partly written, or
// the user stopped before picking the download port). Installing again from
// the download port finishes the job.
export class InstallInterruptedError extends Error {
  constructor(message = INSTALL_INTERRUPTED_MESSAGE, options) {
    super(message, options);
    this.name = 'InstallInterruptedError';
  }
}

async function findAuthorizedDownloadPort(serial) {
  const ports = await serial?.getPorts?.() ?? [];
  return ports.find(isDownloadPort) ?? null;
}

// Firmware running TinyUSB on the native USB port (e.g. "Pixie Pixel Gear" on
// the T-Display-S3) cannot be reset over RTS/DTR. Opening it at 1200 baud
// makes it reboot into ROM download mode, where the chip re-enumerates as its
// built-in USB-Serial/JTAG unit. Web Serial permissions are per device and
// origin, so that unit may need to be picked by the user once.
//
// The browser only opens a port picker right after a click, and the click
// that started the install has usually been used up by then. When
// *requestDownloadPort* is given it is called to get a fresh click from the
// user (e.g. a "Continue installation" button); it resolves with the picked
// port or rejects if the user gives up. Meanwhile the authorized ports keep
// being polled, so a port that shows up on its own also continues.
export async function resolveDownloadPort(port, {
  serial = globalThis.navigator?.serial,
  onStatus = () => {},
  wait = sleep,
  authorizedWaitMs = AUTHORIZED_PORT_WAIT_MS,
  timeoutMs = DOWNLOAD_PORT_TIMEOUT_MS,
  requestDownloadPort,
} = {}) {
  const info = port.getInfo?.() ?? {};
  if (info.usbVendorId !== ESPRESSIF_USB_VENDOR_ID || isDownloadPort(port)) {
    return port;
  }

  onStatus('Restarting the board into download mode');
  try {
    await port.open({ baudRate: 1200 });
  } catch {
    // The board may reboot before open() settles.
  }
  await port.close().catch(() => {});

  const settle = async (downloadPort) => {
    await wait(500);
    return downloadPort;
  };

  let waited = 0;
  for (; waited < authorizedWaitMs; waited += DOWNLOAD_PORT_POLL_MS) {
    await wait(DOWNLOAD_PORT_POLL_MS);
    const downloadPort = await findAuthorizedDownloadPort(serial);
    if (downloadPort) return settle(downloadPort);
  }

  if (serial?.requestPort) {
    onStatus('Choose "USB JTAG/serial debug unit" to continue');
    try {
      const picked = await serial.requestPort({ filters: [DOWNLOAD_PORT_FILTER] });
      if (isDownloadPort(picked)) return settle(picked);
    } catch {
      // The picker needs a recent click; ask the user for one below.
    }
  }

  if (requestDownloadPort) {
    onStatus('Select Continue installation to finish');
    const controller = new AbortController();
    const polled = (async () => {
      while (!controller.signal.aborted) {
        await wait(DOWNLOAD_PORT_POLL_MS);
        const downloadPort = await findAuthorizedDownloadPort(serial);
        if (downloadPort) return downloadPort;
      }
      return null;
    })();
    try {
      const picked = await Promise.race([
        polled,
        requestDownloadPort({ signal: controller.signal }),
      ]);
      if (isDownloadPort(picked)) return settle(picked);
    } catch (error) {
      throw new InstallInterruptedError(undefined, { cause: error });
    } finally {
      controller.abort();
    }
    throw new InstallInterruptedError();
  }

  for (; waited < timeoutMs; waited += DOWNLOAD_PORT_POLL_MS) {
    await wait(DOWNLOAD_PORT_POLL_MS);
    const downloadPort = await findAuthorizedDownloadPort(serial);
    if (downloadPort) return settle(downloadPort);
  }
  throw new DownloadPortNotAuthorizedError();
}

// Splits a merged image (written at address 0) into the pieces to flash,
// leaving out NVS partitions so hotspot settings survive an update.
// The merged image fills the gap before the app with 0xFF, which would
// otherwise erase them.
export function flashSegments(image) {
  const skipped = [];
  for (
    let entry = PARTITION_TABLE_OFFSET;
    entry + PARTITION_ENTRY_SIZE <= Math.min(image.length, PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE);
    entry += PARTITION_ENTRY_SIZE
  ) {
    if (image[entry] !== 0xaa || image[entry + 1] !== 0x50) break;
    const type = image[entry + 2];
    const subtype = image[entry + 3];
    const view = new DataView(image.buffer, image.byteOffset + entry + 4, 8);
    const offset = view.getUint32(0, true);
    const size = view.getUint32(4, true);
    if (
      type === PARTITION_TYPE_DATA
      && (subtype === PARTITION_SUBTYPE_NVS || subtype === PARTITION_SUBTYPE_NVS_KEYS)
    ) {
      skipped.push([offset, offset + size]);
    }
  }
  skipped.sort((a, b) => a[0] - b[0]);

  const segments = [];
  let start = 0;
  for (const [skipStart, skipEnd] of skipped) {
    if (skipStart >= image.length) break;
    if (skipStart > start) {
      segments.push({ data: image.subarray(start, skipStart), address: start });
    }
    start = Math.max(start, skipEnd);
  }
  if (start < image.length) {
    segments.push({ data: image.subarray(start), address: start });
  }
  return segments;
}

// Leaves download mode and starts the installed firmware, without the board's
// RST or BOOT buttons.
export async function restartIntoFirmware(loader, chipName) {
  if (chipName === 'ESP32-S3') {
    // A 1200-baud reboot sets FORCE_DOWNLOAD_BOOT, which survives a reset;
    // clear it or the board boots straight back into download mode.
    try {
      await loader.writeReg(
        ESP32S3_RTC_CNTL_OPTION1_REG,
        0,
        ESP32S3_RTC_CNTL_FORCE_DOWNLOAD_BOOT,
      );
    } catch {
      // Still try to reset below.
    }
    // The RTC watchdog resets the chip no matter how the host drives RTS.
    try {
      await loader.writeReg(ESP32S3_RTC_CNTL_WDTWPROTECT_REG, ESP32S3_RTC_CNTL_WDT_WKEY);
      await loader.writeReg(ESP32S3_RTC_CNTL_WDTCONFIG1_REG, 2000);
      await loader.writeReg(
        ESP32S3_RTC_CNTL_WDTCONFIG0_REG,
        (1 << 31) | (5 << 28) | (1 << 8) | 2,
      );
      await loader.writeReg(ESP32S3_RTC_CNTL_WDTWPROTECT_REG, 0);
    } catch {
      // The chip may reset before the last write is acknowledged.
    }
  }
  try {
    await loader.after('custom_reset', false, RESET_SEQUENCE);
  } catch {
    // On native USB the port disappears as soon as the chip resets, so the
    // rest of the sequence fails; the firmware is already written.
  }
}

// Loads the published firmware versions, newest first.
export async function loadFirmwareVersions({ fetchImpl = globalThis.fetch } = {}) {
  if (!fetchImpl) throw new Error('Firmware download is unavailable');
  return firmwareVersions(await fetchJson(fetchImpl, FIRMWARE_MANIFEST_URL));
}

// Installs *version* (the newest published one when it is left out).
export async function flashBundledFirmware({
  port,
  version,
  serial = globalThis.navigator?.serial,
  fetchImpl = globalThis.fetch,
  onProgress = () => {},
  onStatus = () => {},
  requestDownloadPort,
  Loader = ESPLoader,
  SerialTransport = Transport,
  wait = sleep,
}) {
  if (!port) throw new Error('Choose a serial device first');
  if (!fetchImpl) throw new Error('Firmware download is unavailable');

  // Download everything before the board leaves its firmware, so a network
  // problem cannot leave it stuck in download mode.
  onStatus('Loading bundled firmware');
  const versions = firmwareVersions(await fetchJson(fetchImpl, FIRMWARE_MANIFEST_URL));
  const release = version === undefined
    ? versions[0]
    : versions.find((candidate) => candidate.version === version);
  if (!release) throw new Error(`Firmware ${version} is not available`);
  const manifest = { version: release.version };
  const { builds } = release;
  const images = new Map();
  for (const build of builds) {
    const firmware = await fetchBytes(fetchImpl, build.path);
    if (firmware.byteLength !== build.size) {
      throw new Error(
        `Bundled firmware size is invalid (expected ${build.size}, received ${firmware.byteLength})`,
      );
    }
    images.set(build, firmware);
  }

  const flashPort = await resolveDownloadPort(port, {
    serial, onStatus, wait, requestDownloadPort,
  });
  const terminal = {
    clean() {},
    write() {},
    writeLine(message) {
      if (message) onStatus(message);
    },
  };

  let serialTransport;
  let loader;
  onStatus('Entering ESP32 bootloader');
  for (let attempt = 1; ; attempt += 1) {
    serialTransport = new SerialTransport(flashPort, false);
    loader = new Loader({ transport: serialTransport, baudrate: 921600, terminal });
    try {
      await loader.main();
      break;
    } catch (error) {
      await serialTransport.disconnect().catch(() => {});
      // The download port can take a moment to accept connections after
      // re-enumerating.
      if (attempt >= BOOTLOADER_ATTEMPTS) {
        throw new InstallInterruptedError(
          `The board did not respond in download mode (${error.message}). Select Install firmware again to retry.`,
          { cause: error },
        );
      }
      await wait(1000);
    }
  }

  let chipName;
  try {
    chipName = loader.chip?.CHIP_NAME;
    const build = builds.find((candidate) => candidate.chip === chipName);
    if (!build) {
      // Nothing was written; put the board back on its own firmware.
      await restartIntoFirmware(loader, chipName);
      const supported = builds.map((candidate) => candidate.chip).join(' or ');
      throw new Error(
        `This firmware requires an ${supported}; detected ${chipName ?? 'an unsupported chip'}`,
      );
    }

    const segments = flashSegments(images.get(build));
    const totalBytes = segments.reduce((sum, segment) => sum + segment.data.length, 0);
    const offsets = [];
    segments.reduce((sum, segment) => {
      offsets.push(sum);
      return sum + segment.data.length;
    }, 0);

    const target = build.board ? ` for ${build.board}` : '';
    onStatus(`Installing Cyberclip firmware ${manifest.version}${target}`);
    try {
      await loader.writeFlash({
        fileArray: segments,
        flashMode: build.flashMode,
        flashFreq: build.flashFreq,
        flashSize: '16MB',
        eraseAll: false,
        compress: true,
        reportProgress(fileIndex, written, total) {
          const fraction = total > 0 ? written / total : 0;
          const done = offsets[fileIndex] + fraction * segments[fileIndex].data.length;
          onProgress(totalBytes > 0 ? (done / totalBytes) * 100 : 0);
        },
      });
    } catch (error) {
      // The flash is only partly written, so the board must stay in download
      // mode, where installing again finishes the job.
      throw new InstallInterruptedError(undefined, { cause: error });
    }
    onProgress(100);
    onStatus(`Restarting ${chipName}`);
    await restartIntoFirmware(loader, chipName);
    return { ...manifest, chip: chipName, board: build.board };
  } finally {
    await serialTransport.disconnect().catch(() => {});
  }
}

async function fetchJson(fetchImpl, url) {
  const response = await fetchImpl(url, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`Bundled firmware metadata could not be loaded (${response.status})`);
  return response.json();
}

async function fetchBytes(fetchImpl, url) {
  const response = await fetchImpl(url, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`Bundled firmware could not be loaded (${response.status})`);
  return new Uint8Array(await response.arrayBuffer());
}

const FLASH_MODES = new Set(['keep', 'qio', 'qout', 'dio', 'dout']);
const FLASH_FREQS = new Set(['keep', '40m', '26m', '20m', '80m']);

function isValidImage(entry) {
  return Boolean(entry)
    && typeof entry.path === 'string'
    && /^\/firmware\/[^/]+\.bin$/.test(entry.path)
    && Number.isSafeInteger(entry.address)
    && entry.address === 0
    && Number.isSafeInteger(entry.size)
    && entry.size > 0;
}

const VERSION_PATTERN = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;

export function compareVersions(a, b) {
  const left = a.split('.').map(Number);
  const right = b.split('.').map(Number);
  for (let index = 0; index < 3; index += 1) {
    if (left[index] !== right[index]) return left[index] - right[index];
  }
  return 0;
}

// Returns every published version as { version, builds, latest }, newest
// first. Manifests without a `versions` list only offer the top-level build.
export function firmwareVersions(manifest) {
  const listed = Array.isArray(manifest?.versions) && manifest.versions.length > 0
    ? manifest.versions
    : [manifest];
  const seen = new Set();
  const versions = listed.map((entry) => {
    if (!VERSION_PATTERN.test(entry?.version ?? '') || seen.has(entry.version)) {
      throw new Error('Bundled firmware metadata is invalid');
    }
    seen.add(entry.version);
    return { version: entry.version, builds: firmwareBuilds(entry), latest: false };
  });
  versions.sort((a, b) => compareVersions(b.version, a.version));
  versions[0].latest = true;
  return versions;
}

// Returns the flashable images listed in a manifest entry's `builds`.
export function firmwareBuilds(manifest) {
  if (!manifest || typeof manifest.version !== 'string') {
    throw new Error('Bundled firmware metadata is invalid');
  }
  const builds = manifest.builds;
  if (!Array.isArray(builds) || builds.length === 0) {
    throw new Error('Bundled firmware metadata is invalid');
  }
  return builds.map((build) => {
    const normalized = {
      chip: build?.chip,
      board: typeof build?.board === 'string' ? build.board : undefined,
      path: build?.path,
      address: build?.address,
      size: build?.size,
      flashMode: build?.flashMode ?? 'keep',
      flashFreq: build?.flashFreq ?? 'keep',
    };
    if (
      typeof normalized.chip !== 'string'
      || !isValidImage(normalized)
      || !FLASH_MODES.has(normalized.flashMode)
      || !FLASH_FREQS.has(normalized.flashFreq)
    ) {
      throw new Error('Bundled firmware metadata is invalid');
    }
    return normalized;
  });
}
