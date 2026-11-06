import { describe, expect, it, vi } from 'vitest';
import {
  DOWNLOAD_PORT_FILTER,
  DownloadPortNotAuthorizedError,
  InstallInterruptedError,
  flashBundledFirmware,
  flashSegments,
  resolveDownloadPort,
  restartIntoFirmware,
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

  it('rejects a chip without a matching firmware image and restarts it', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest()))
      .mockResolvedValueOnce(response([1, 2, 3, 4]));
    const after = vi.fn().mockResolvedValue();
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32' };

      main = vi.fn().mockResolvedValue('ESP32');

      writeFlash = vi.fn();

      after = after;
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
    expect(after).toHaveBeenCalled();
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

// A merged image with a partition table at 0x8000 listing NVS at 0x9000.
function mergedImage(length = 0x10010) {
  const image = new Uint8Array(length).fill(0xff);
  const entry = (index, type, subtype, offset, size) => {
    const at = 0x8000 + index * 32;
    image.set([0xaa, 0x50, type, subtype], at);
    const view = new DataView(image.buffer, at + 4, 8);
    view.setUint32(0, offset, true);
    view.setUint32(4, size, true);
  };
  entry(0, 0x01, 0x02, 0x9000, 0x5000); // nvs
  entry(1, 0x01, 0x00, 0xe000, 0x2000); // otadata
  entry(2, 0x00, 0x10, 0x10000, 0x300000); // app0
  return image;
}

describe('flash segments', () => {
  it('leaves the NVS partition (WiFi settings) out of the write', () => {
    const image = mergedImage();
    const segments = flashSegments(image);
    expect(segments.map(({ address, data }) => [address, data.length])).toEqual([
      [0, 0x9000],
      [0xe000, image.length - 0xe000],
    ]);
    expect(segments[1].data[0]).toBe(image[0xe000]);
  });

  it('writes an image without a partition table as one piece', () => {
    const image = Uint8Array.from([1, 2, 3]);
    expect(flashSegments(image)).toEqual([{ data: image, address: 0 }]);
  });
});

describe('recovery without the board buttons', () => {
  const pixie = () => ({
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x0002 }),
    open: vi.fn().mockResolvedValue(),
    close: vi.fn().mockResolvedValue(),
  });
  const jtag = { getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }) };
  const noGesture = () => new DOMException('No gesture', 'SecurityError');

  it('waits for a fresh click when the picker cannot open, instead of failing', async () => {
    const serial = {
      getPorts: vi.fn().mockResolvedValue([]),
      requestPort: vi.fn().mockRejectedValue(noGesture()),
    };
    const requestDownloadPort = vi.fn().mockResolvedValue(jtag);

    const result = await resolveDownloadPort(pixie(), {
      serial, wait: async () => {}, timeoutMs: 500, requestDownloadPort,
    });

    expect(requestDownloadPort).toHaveBeenCalledWith({ signal: expect.any(AbortSignal) });
    expect(result).toBe(jtag);
  });

  it('continues on its own when the download port becomes authorized', async () => {
    let calls = 0;
    const serial = {
      getPorts: vi.fn().mockImplementation(async () => {
        calls += 1;
        return calls > 20 ? [jtag] : [];
      }),
      requestPort: vi.fn().mockRejectedValue(noGesture()),
    };
    let signal;
    const requestDownloadPort = vi.fn().mockImplementation((options) => {
      signal = options.signal;
      return new Promise(() => {});
    });

    const result = await resolveDownloadPort(pixie(), {
      serial, wait: async () => {}, requestDownloadPort,
    });

    expect(result).toBe(jtag);
    expect(signal.aborted).toBe(true);
  });

  it('reports a cancelled install as interrupted', async () => {
    const serial = {
      getPorts: vi.fn().mockResolvedValue([]),
      requestPort: vi.fn().mockRejectedValue(noGesture()),
    };
    const requestDownloadPort = vi.fn()
      .mockRejectedValue(new DOMException('Installation cancelled', 'AbortError'));

    await expect(resolveDownloadPort(pixie(), {
      serial, wait: async () => {}, requestDownloadPort,
    })).rejects.toBeInstanceOf(InstallInterruptedError);
  });

  it('downloads the firmware before restarting the board', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest()))
      .mockResolvedValueOnce(response([1, 2]));
    const board = pixie();

    await expect(flashBundledFirmware({
      port: board,
      fetchImpl,
      Loader: class {},
      SerialTransport: class {},
    })).rejects.toThrow('size is invalid');
    expect(board.open).not.toHaveBeenCalled();
  });

  function fakeTransport() {
    return class {
      disconnect = vi.fn().mockResolvedValue();
    };
  }

  it('retries the bootloader connection', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest({ size: 3 })))
      .mockResolvedValueOnce(response([1, 2, 3]));
    const main = vi.fn()
      .mockRejectedValueOnce(new Error('Failed to connect'))
      .mockResolvedValue('ESP32-S3');
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-S3' };

      main = main;

      writeFlash = vi.fn().mockResolvedValue();

      writeReg = vi.fn().mockResolvedValue();

      after = vi.fn().mockResolvedValue();
    }

    await flashBundledFirmware({
      port: jtag,
      fetchImpl,
      Loader: FakeLoader,
      SerialTransport: fakeTransport(),
      wait: async () => {},
    });
    expect(main).toHaveBeenCalledTimes(2);
  });

  it('keeps the board in download mode when writing is interrupted', async () => {
    const fetchImpl = vi.fn()
      .mockResolvedValueOnce(response(s3Manifest({ size: 3 })))
      .mockResolvedValueOnce(response([1, 2, 3]));
    const writeReg = vi.fn().mockResolvedValue();
    class FakeLoader {
      chip = { CHIP_NAME: 'ESP32-S3' };

      main = vi.fn().mockResolvedValue('ESP32-S3');

      writeFlash = vi.fn().mockRejectedValue(new Error('Timeout'));

      writeReg = writeReg;

      after = vi.fn();
    }

    await expect(flashBundledFirmware({
      port: jtag,
      fetchImpl,
      Loader: FakeLoader,
      SerialTransport: fakeTransport(),
    })).rejects.toBeInstanceOf(InstallInterruptedError);
    expect(writeReg).not.toHaveBeenCalled();
  });

  it('restarts with the RTC watchdog after clearing forced download boot', async () => {
    const writeReg = vi.fn().mockResolvedValue();
    const after = vi.fn().mockResolvedValue();
    await restartIntoFirmware({ writeReg, after }, 'ESP32-S3');
    expect(writeReg.mock.calls[0]).toEqual([0x6000812c, 0, 0x1]);
    expect(writeReg).toHaveBeenCalledWith(0x600080b0, 0x50d83aa1);
    expect(writeReg).toHaveBeenCalledWith(0x60008098, (1 << 31) | (5 << 28) | (1 << 8) | 2);
    expect(after).toHaveBeenCalled();
  });
});
