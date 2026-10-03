import { describe, expect, it, vi } from 'vitest';
import {
  DOWNLOAD_PORT_FILTER,
  DownloadPortNotAuthorizedError,
  flashBundledFirmware,
  resolveDownloadPort,
} from './flasher.js';

function response(body, { ok = true, status = 200 } = {}) {
  return {
    ok,
    status,
    json: async () => body,
    arrayBuffer: async () => Uint8Array.from(body).buffer,
  };
}

function s3Manifest({ version = '2.0.15', size = 4 } = {}) {
  return {
    version,
    builds: [{
      chip: 'ESP32-S3',
      board: 'LilyGO T-Display-S3',
      path: `/firmware/cyberclip-lilygo-t-display-s3-${version}.bin`,
      address: 0,
      size,
      flashMode: 'keep',
      flashFreq: 'keep',
    }],
  };
}

describe('bundled firmware flashing', () => {
  it('flashes the T-Display-S3 image, resets, and disconnects', async () => {
    const firmware = [9, 8, 7];
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest({ size: firmware.length })))
      .mockResolvedValueOnce(response(firmware));
    const writeFlash = vi.fn().mockImplementation(async ({ reportProgress }) => {
      reportProgress(0, 3, 3);
    });
    const after = vi.fn().mockRejectedValue(new Error('Invalid custom reset sequence'));
    const writeReg = vi.fn().mockResolvedValue();
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-S3' };

      main = vi.fn().mockResolvedValue('ESP32-S3');

      writeFlash = writeFlash;

      writeReg = writeReg;

      after = after;
    }
    const disconnect = vi.fn().mockResolvedValue();
    class FakeTransport {
      constructor(port) {
        expect(port).toBe('serial-port');
      }

      disconnect = disconnect;
    }
    const onProgress = vi.fn();

    const result = await flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      onProgress,
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
      eraseAll: false,
    }));
    expect(onProgress).toHaveBeenLastCalledWith(100);
    expect(writeReg).toHaveBeenCalledWith(0x6000812c, 0, 0x1);
    expect(writeReg.mock.invocationCallOrder[0])
      .toBeLessThan(after.mock.invocationCallOrder[0]);
    expect(after).toHaveBeenCalledWith('custom_reset', false, 'D0|R1|W200|R0|W500');
    expect(disconnect).toHaveBeenCalled();
    expect(result).toMatchObject({
      version: '2.0.15',
      chip: 'ESP32-S3',
      board: 'LilyGO T-Display-S3',
    });
  });

  it('rejects a manifest without a builds list', async () => {
    const fetchImpl = vi.fn().mockResolvedValueOnce(response({
      version: '2.0.3',
      path: '/firmware/cyberclip-2.0.3.bin',
      address: 0,
      size: 4,
    }));

    await expect(flashBundledFirmware({
      port: 'serial-port',
      fetchImpl,
      Loader: class {},
      SerialTransport: class {},
    })).rejects.toThrow('Bundled firmware metadata is invalid');
  });

  it('rejects a chip without a matching firmware image', async () => {
    const fetchImpl = vi.fn().mockResolvedValueOnce(response(s3Manifest()));
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
    })).rejects.toThrow('requires an ESP32-S3; detected ESP32');
  });

  it('rejects a firmware image whose size does not match its manifest', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest({ size: 5 })))
      .mockResolvedValueOnce(response([1, 2, 3, 4]));
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-S3' };

      main = vi.fn().mockResolvedValue('ESP32-S3');
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

describe('native USB download port', () => {
  const port = (usbVendorId, usbProductId) => ({
    getInfo: () => ({ usbVendorId, usbProductId }),
    open: vi.fn().mockResolvedValue(),
    close: vi.fn().mockResolvedValue(),
  });

  it('uses the USB-Serial/JTAG port directly', async () => {
    const jtag = port(0x303a, 0x1001);
    expect(await resolveDownloadPort(jtag)).toBe(jtag);
    expect(jtag.open).not.toHaveBeenCalled();
  });

  it('reboots TinyUSB firmware with a 1200 baud touch and returns the JTAG port', async () => {
    const pixie = port(0x303a, 0x0002);
    const jtag = port(0x303a, 0x1001);
    const serial = {
      getPorts: vi.fn()
        .mockResolvedValueOnce([])
        .mockResolvedValueOnce([jtag]),
    };

    const result = await resolveDownloadPort(pixie, { serial, wait: async () => {} });

    expect(pixie.open).toHaveBeenCalledWith({ baudRate: 1200 });
    expect(pixie.close).toHaveBeenCalled();
    expect(result).toBe(jtag);
  });

  it('asks the user to pick the download port when it is not authorized', async () => {
    const pixie = port(0x303a, 0x0002);
    const serial = { getPorts: vi.fn().mockResolvedValue([]) };

    await expect(resolveDownloadPort(pixie, {
      serial,
      wait: async () => {},
      timeoutMs: 500,
    })).rejects.toThrow('USB JTAG/serial debug unit');
  });

  it('opens a picker filtered to the download port when it is not authorized', async () => {
    const pixie = port(0x303a, 0x0002);
    const jtag = port(0x303a, 0x1001);
    const serial = {
      getPorts: vi.fn().mockResolvedValue([]),
      requestPort: vi.fn().mockResolvedValue(jtag),
    };

    const result = await resolveDownloadPort(pixie, { serial, wait: async () => {} });

    expect(serial.requestPort).toHaveBeenCalledWith({ filters: [DOWNLOAD_PORT_FILTER] });
    expect(result).toBe(jtag);
  });

  it('reports an unauthorized download port when the picker cannot open', async () => {
    const pixie = port(0x303a, 0x0002);
    const serial = {
      getPorts: vi.fn().mockResolvedValue([]),
      requestPort: vi.fn().mockRejectedValue(new DOMException('No gesture', 'SecurityError')),
    };

    await expect(resolveDownloadPort(pixie, {
      serial,
      wait: async () => {},
      timeoutMs: 500,
    })).rejects.toBeInstanceOf(DownloadPortNotAuthorizedError);
  });
});