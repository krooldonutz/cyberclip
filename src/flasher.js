import { ESPLoader, Transport } from 'esptool-js';

export const FIRMWARE_MANIFEST_URL = '/firmware/manifest.json';

// Reset sequences after flashing. Native USB-Serial/JTAG (ESP32-S3) needs a
// longer reset pulse than a CH340-style USB-UART bridge.
const RESET_SEQUENCES = {
  ESP32: 'D0|R1|W100|R0|W500',
  'ESP32-S3': 'D0|R1|W200|R0|W500',
};

export const ESPRESSIF_USB_VENDOR_ID = 0x303a;
export const ESP32S3_RTC_CNTL_OPTION1_REG = 0x6000812c;
export const ESP32S3_RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1;
export const USB_JTAG_SERIAL_PRODUCT_ID = 0x1001;
export const DOWNLOAD_PORT_FILTER = Object.freeze({
  usbVendorId: ESPRESSIF_USB_VENDOR_ID,
  usbProductId: USB_JTAG_SERIAL_PRODUCT_ID,
});
export const DOWNLOAD_PORT_NOT_AUTHORIZED_MESSAGE =
  'The board restarted into download mode. Select Install firmware again and choose "USB JTAG/serial debug unit".';
// How long to wait for an already-authorized download port before asking the
// user to pick it (the click that started the install is still active then).
const AUTHORIZED_PORT_WAIT_MS = 2500;
const DOWNLOAD_PORT_TIMEOUT_MS = 10000;
const DOWNLOAD_PORT_POLL_MS = 250;

const sleep = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });

export function isDownloadPort(port) {
  const info = port?.getInfo?.() ?? {};
  return info.usbVendorId === ESPRESSIF_USB_VENDOR_ID
    && info.usbProductId === USB_JTAG_SERIAL_PRODUCT_ID;
}

export class DownloadPortNotAuthorizedError extends Error {
  constructor() {
    super(DOWNLOAD_PORT_NOT_AUTHORIZED_MESSAGE);
    this.name = 'DownloadPortNotAuthorizedError';
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
export async function resolveDownloadPort(port, {
  serial = globalThis.navigator?.serial,
  onStatus = () => {},
  wait = sleep,
  authorizedWaitMs = AUTHORIZED_PORT_WAIT_MS,
  timeoutMs = DOWNLOAD_PORT_TIMEOUT_MS,
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

  let waited = 0;
  for (; waited < authorizedWaitMs; waited += DOWNLOAD_PORT_POLL_MS) {
    await wait(DOWNLOAD_PORT_POLL_MS);
    const downloadPort = await findAuthorizedDownloadPort(serial);
    if (downloadPort) {
      await wait(500);
      return downloadPort;
    }
  }

  if (serial?.requestPort) {
    onStatus('Choose "USB JTAG/serial debug unit" to continue');
    try {
      const picked = await serial.requestPort({ filters: [DOWNLOAD_PORT_FILTER] });
      if (isDownloadPort(picked)) {
        await wait(500);
        return picked;
      }
    } catch {
      // The picker needs a recent click; fall back to waiting, then asking
      // the user to select Install firmware again.
    }
  }

  for (; waited < timeoutMs; waited += DOWNLOAD_PORT_POLL_MS) {
    await wait(DOWNLOAD_PORT_POLL_MS);
    const downloadPort = await findAuthorizedDownloadPort(serial);
    if (downloadPort) {
      await wait(500);
      return downloadPort;
    }
  }
  throw new DownloadPortNotAuthorizedError();
}

export async function flashBundledFirmware({
  port,
  serial = globalThis.navigator?.serial,
  fetchImpl = globalThis.fetch,
  onProgress = () => {},
  onStatus = () => {},
  Loader = ESPLoader,
  SerialTransport = Transport,
  wait = sleep,
}) {
  if (!port) throw new Error('Choose an ESP32 serial device first');
  if (!fetchImpl) throw new Error('Firmware download is unavailable');

  onStatus('Loading bundled firmware');
  const manifest = await fetchJson(fetchImpl, FIRMWARE_MANIFEST_URL);
  const builds = firmwareBuilds(manifest);

  const flashPort = await resolveDownloadPort(port, { serial, onStatus, wait });
  const serialTransport = new SerialTransport(flashPort, false);
  const loader = new Loader({
    transport: serialTransport,
    baudrate: 921600,
    terminal: {
      clean() {},
      write() {},
      writeLine(message) {
        if (message) onStatus(message);
      },
    },
  });

  try {
    onStatus('Entering ESP32 bootloader');
    await loader.main();
    const chipName = loader.chip?.CHIP_NAME;
    const build = builds.find((candidate) => candidate.chip === chipName);
    if (!build) {
      const supported = builds.map((candidate) => candidate.chip).join(' or ');
      throw new Error(
        `This firmware requires an ${supported}; detected ${chipName ?? 'an unsupported chip'}`,
      );
    }

    const firmware = await fetchBytes(fetchImpl, build.path);
    if (firmware.byteLength !== build.size) {
      throw new Error(
        `Bundled firmware size is invalid (expected ${build.size}, received ${firmware.byteLength})`,
      );
    }

    const target = build.board ? ` for ${build.board}` : '';
    onStatus(`Installing Cyberclip firmware ${manifest.version}${target}`);
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: build.address }],
      flashMode: build.flashMode,
      flashFreq: build.flashFreq,
      flashSize: '16MB',
      eraseAll: false,
      compress: true,
      reportProgress(_fileIndex, written, total) {
        onProgress(total > 0 ? (written / total) * 100 : 0);
      },
    });
    onProgress(100);
    onStatus(`Resetting ${chipName}`);
    if (chipName === 'ESP32-S3') {
      // A 1200-baud reboot sets FORCE_DOWNLOAD_BOOT, which survives a reset;
      // clear it or the board boots straight back into download mode.
      await loader.writeReg(
        ESP32S3_RTC_CNTL_OPTION1_REG,
        0,
        ESP32S3_RTC_CNTL_FORCE_DOWNLOAD_BOOT,
      );
    }
    try {
      await loader.after('custom_reset', false, RESET_SEQUENCES[chipName] ?? RESET_SEQUENCES.ESP32);
    } catch {
      // On native USB the port disappears as soon as the chip resets, so the
      // rest of the sequence fails; the firmware is already written.
    }
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

// Returns the flashable images listed in the manifest. Manifests without a
// `builds` list describe a single ESP32 image in their top-level fields.
export function firmwareBuilds(manifest) {
  if (!manifest || typeof manifest.version !== 'string') {
    throw new Error('Bundled firmware metadata is invalid');
  }
  const builds = Array.isArray(manifest.builds)
    ? manifest.builds
    : [{ chip: 'ESP32', path: manifest.path, address: manifest.address, size: manifest.size }];
  if (builds.length === 0) throw new Error('Bundled firmware metadata is invalid');
  return builds.map((build) => {
    const normalized = {
      chip: build?.chip,
      board: typeof build?.board === 'string' ? build.board : undefined,
      path: build?.path,
      address: build?.address,
      size: build?.size,
      flashMode: build?.flashMode ?? 'dio',
      flashFreq: build?.flashFreq ?? '40m',
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
