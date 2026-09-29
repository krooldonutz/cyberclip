import { ESPLoader, Transport } from 'esptool-js';

export const FIRMWARE_MANIFEST_URL = '/firmware/manifest.json';

// Reset sequences after flashing. Native USB-Serial/JTAG (ESP32-S3) needs a
// longer reset pulse than a CH340-style USB-UART bridge.
const RESET_SEQUENCES = {
  ESP32: 'D0|R1|W100|R0|W500',
  'ESP32-S3': 'D0|R1|W200|R0|W500',
};

export async function flashBundledFirmware({
  port,
  fetchImpl = globalThis.fetch,
  onProgress = () => {},
  onStatus = () => {},
  Loader = ESPLoader,
  SerialTransport = Transport,
}) {
  if (!port) throw new Error('Choose an ESP32 serial device first');
  if (!fetchImpl) throw new Error('Firmware download is unavailable');

  onStatus('Loading bundled firmware');
  const manifest = await fetchJson(fetchImpl, FIRMWARE_MANIFEST_URL);
  const builds = firmwareBuilds(manifest);

  const serialTransport = new SerialTransport(port, false);
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
    await loader.after('custom_reset', false, RESET_SEQUENCES[chipName] ?? RESET_SEQUENCES.ESP32);
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
