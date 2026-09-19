import { describe, expect, it } from 'vitest';
import { Command, PacketDecoder, encodePacket } from './protocol.js';
import { SerialTransport } from './serial.js';

function createFakeDevice() {
  let readController;
  const requestDecoder = new PacketDecoder();
  const readable = new ReadableStream({
    start(controller) {
      readController = controller;
    },
  });
  const writable = new WritableStream({
    write(bytes) {
      for (const request of requestDecoder.push(bytes)) {
        if (request.command !== Command.HELLO) continue;
        const name = new TextEncoder().encode('Test display');
        const payload = new Uint8Array(20 + name.length);
        const view = new DataView(payload.buffer);
        view.setUint16(0, 170, true);
        view.setUint16(2, 320, true);
        view.setUint16(4, 4090, true);
        view.setUint32(6, 128 * 1024, true);
        view.setUint8(10, 0x0f);
        view.setUint8(11, 0x01);
        view.setUint8(12, 1);
        view.setUint8(15, 1);
        view.setUint32(16, 8 * 1024 * 1024, true);
        payload.set(name, 20);
        readController.enqueue(encodePacket(
          Command.HELLO_RESPONSE,
          request.sequence,
          payload,
        ));
      }
    },
  });
  return {
    readable,
    writable,
    async open() {},
    async close() {},
  };
}

describe('SerialTransport', () => {
  it('requires a successful capability handshake', async () => {
    const port = createFakeDevice();
    const serial = {
      requestPort: async () => port,
      addEventListener() {},
      removeEventListener() {},
    };
    const transport = new SerialTransport({ serial });

    const capabilities = await transport.connect();
    expect(capabilities).toMatchObject({
      width: 170,
      height: 320,
      deviceName: 'Test display',
      firmwareVersion: '1.0.0',
    });

    await transport.disconnect();
    expect(transport.connected).toBe(false);
  });
});
