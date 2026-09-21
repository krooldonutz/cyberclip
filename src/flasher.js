import { ESPLoader, Transport } from 'esptool-js';

export const FIRMWARE_MANIFEST_URL = '/firmware/manifest.json';
export const FLASH_BAUD_RATE = 115200;

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
  validateManifest(manifest);
  const firmware = await fetchBytes(fetchImpl, manifest.path);
  if (firmware.byteLength !== manifest.size) {
    throw new Error(
      `Bundled firmware size is invalid (expected ${manifest.size}, received ${firmware.byteLength})`,
    );
  }

  const serialTransport = new SerialTransport(port, false);
  const loader = new Loader({
    transport: serialTransport,
    baudrate: FLASH_BAUD_RATE,
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
    if (loader.chip?.CHIP_NAME !== 'ESP32') {
      throw new Error(
        `This firmware requires an ESP32; detected ${loader.chip?.CHIP_NAME ?? 'an unsupported chip'}`,
      );
    }

    onStatus(`Installing Cyberclip firmware ${manifest.version}`);
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: manifest.address }],
      flashMode: 'dio',
      flashFreq: '40m',
      flashSize: '16MB',
      eraseAll: false,
      compress: true,
      reportProgress(_fileIndex, written, total) {
        onProgress(total > 0 ? (written / total) * 100 : 0);
      },
    });
    onProgress(100);
    onStatus('Resetting ESP32');
    await loader.after('custom_reset', false, 'D0|R1|W100|R0|W500');
    return manifest;
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

function validateManifest(manifest) {
  if (
    !manifest
    || typeof manifest.version !== 'string'
    || typeof manifest.path !== 'string'
    || !manifest.path.startsWith('/firmware/')
    || !Number.isSafeInteger(manifest.address)
    || manifest.address !== 0
    || !Number.isSafeInteger(manifest.size)
    || manifest.size <= 0
  ) {
    throw new Error('Bundled firmware metadata is invalid');
  }
}
