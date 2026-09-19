import { describe, expect, it, vi } from 'vitest';
import { flashBundledFirmware } from './flasher.js';

function response(body, { ok = true, status = 200 } = {}) {
  return {
    ok,
    status,
    json: async () => body,
    arrayBuffer: async () => Uint8Array.from(body).buffer,
  };
}

describe('bundled firmware flashing', () => {
  it('loads, validates, flashes, resets, and disconnects', async () => {
    const firmware = [1, 2, 3, 4];
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response({
        version: '2.0.2',
        path: '/firmware/cyberclip-2.0.2.bin',
        address: 0,
        size: firmware.length,
      }))
      .mockResolvedValueOnce(response(firmware));
    const disconnect = vi.fn().mockResolvedValue();
    class FakeTransport {
      constructor(port) {
        expect(port).toBe('serial-port');
      }

      disconnect = disconnect;
    }
    const writeFlash = vi.fn().mockImplementation(async ({ reportProgress }) => {
      reportProgress(0, 4, 4);
    });
    const after = vi.fn().mockResolvedValue();
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32' };

      main = vi.fn().mockResolvedValue('ESP32');

      writeFlash = writeFlash;

      after = after;
    }
    const onProgress = vi.fn();

    const manifest = await flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      onProgress,
      Loader: FakeLoader,
      SerialTransport: FakeTransport,
    });

    expect(manifest.version).toBe('2.0.2');
    expect(writeFlash).toHaveBeenCalledWith(expect.objectContaining({
      fileArray: [{ data: Uint8Array.from(firmware), address: 0 }],
      eraseAll: false,
    }));
    expect(onProgress).toHaveBeenLastCalledWith(100);
    expect(after).toHaveBeenCalledWith(
      'custom_reset',
      false,
      'D0|R1|W100|R0|W500',
    );
    expect(disconnect).toHaveBeenCalled();
  });

  it('rejects a firmware image whose size does not match its manifest', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response({
        version: '2.0.2',
        path: '/firmware/cyberclip-2.0.2.bin',
        address: 0,
        size: 5,
      }))
      .mockResolvedValueOnce(response([1, 2, 3, 4]));

    await expect(flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
    })).rejects.toThrow('size is invalid');
  });
});
