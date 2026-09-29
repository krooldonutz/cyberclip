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
        version: '2.0.3',
        path: '/firmware/cyberclip-2.0.3.bin',
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

    expect(manifest.version).toBe('2.0.3');
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

  it('selects the ESP32-S3 image for a LilyGO T-Display-S3', async () => {
    const firmware = [9, 8, 7];
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response({
        version: '2.0.15',
        path: '/firmware/cyberclip-2.0.15.bin',
        address: 0,
        size: 4,
        builds: [
          {
            chip: 'ESP32',
            board: 'ideaspark ESP32 ST7789',
            path: '/firmware/cyberclip-2.0.15.bin',
            address: 0,
            size: 4,
            flashMode: 'dio',
            flashFreq: '40m',
          },
          {
            chip: 'ESP32-S3',
            board: 'LilyGO T-Display-S3',
            path: '/firmware/cyberclip-lilygo-t-display-s3-2.0.15.bin',
            address: 0,
            size: firmware.length,
            flashMode: 'keep',
            flashFreq: 'keep',
          },
        ],
      }))
      .mockResolvedValueOnce(response(firmware));
    const writeFlash = vi.fn().mockResolvedValue();
    const after = vi.fn().mockResolvedValue();
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-S3' };

      main = vi.fn().mockResolvedValue('ESP32-S3');

      writeFlash = writeFlash;

      after = after;
    }
    class FakeTransport {
      disconnect = vi.fn().mockResolvedValue();
    }

    const result = await flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      Loader: FakeLoader,
      SerialTransport: FakeTransport,
    });

    expect(fetchImpl).toHaveBeenLastCalledWith(
      '/firmware/cyberclip-lilygo-t-display-s3-2.0.15.bin',
      expect.anything(),
    );
    expect(writeFlash).toHaveBeenCalledWith(expect.objectContaining({
      fileArray: [{ data: Uint8Array.from(firmware), address: 0 }],
      flashMode: 'keep',
      flashFreq: 'keep',
    }));
    expect(after).toHaveBeenCalledWith('custom_reset', false, 'D0|R1|W200|R0|W500');
    expect(result).toMatchObject({ chip: 'ESP32-S3', board: 'LilyGO T-Display-S3' });
  });

  it('rejects a chip without a matching firmware image', async () => {
    const fetchImpl = vi.fn().mockResolvedValueOnce(response({
      version: '2.0.3',
      path: '/firmware/cyberclip-2.0.3.bin',
      address: 0,
      size: 4,
    }));
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-C3' };

      main = vi.fn().mockResolvedValue('ESP32-C3');
    }
    class FakeTransport {
      disconnect = vi.fn().mockResolvedValue();
    }

    await expect(flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      Loader: FakeLoader,
      SerialTransport: FakeTransport,
    })).rejects.toThrow('requires an ESP32; detected ESP32-C3');
  });

  it('rejects a firmware image whose size does not match its manifest', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response({
        version: '2.0.3',
        path: '/firmware/cyberclip-2.0.3.bin',
        address: 0,
        size: 5,
      }))
      .mockResolvedValueOnce(response([1, 2, 3, 4]));
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32' };

      main = vi.fn().mockResolvedValue('ESP32');
    }
    class FakeTransport {
      disconnect = vi.fn().mockResolvedValue();
    }

    await expect(flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      Loader: FakeLoader,
      SerialTransport: FakeTransport,
    })).rejects.toThrow('size is invalid');
  });
});