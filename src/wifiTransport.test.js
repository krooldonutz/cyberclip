import { describe, expect, it } from 'vitest';
import { Command, PacketDecoder, encodePacket } from './protocol.js';
import { WifiTransport } from './wifiTransport.js';

function createFakeWebSocketImpl() {
  const requestDecoder = new PacketDecoder();

  class FakeWebSocket {
    static sent = [];

    constructor(url) {
      this.url = url;
      this.readyState = FakeWebSocket.CONNECTING;
      this.binaryType = '';
      this.listeners = { open: [], message: [], close: [], error: [] };
      queueMicrotask(() => {
        this.readyState = FakeWebSocket.OPEN;
        this.emit('open', {});
      });
    }

    addEventListener(type, callback) {
      this.listeners[type].push(callback);
    }

    emit(type, event) {
      for (const callback of this.listeners[type]) callback(event);
    }

    send(data) {
      const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
      FakeWebSocket.sent.push(bytes);
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
        const response = encodePacket(Command.HELLO_RESPONSE, request.sequence, payload);
        queueMicrotask(() => this.emit('message', { data: response.buffer }));
      }
    }

    close() {
      if (this.readyState === FakeWebSocket.CLOSED) return;
      this.readyState = FakeWebSocket.CLOSED;
      this.emit('close', {});
    }
  }
  FakeWebSocket.CONNECTING = 0;
  FakeWebSocket.OPEN = 1;
  FakeWebSocket.CLOSING = 2;
  FakeWebSocket.CLOSED = 3;

  return FakeWebSocket;
}

describe('WifiTransport', () => {
  it('requires a successful capability handshake', async () => {
    const transport = new WifiTransport({ WebSocketImpl: createFakeWebSocketImpl() });

    const capabilities = await transport.connect({ host: '192.168.4.1' });
    expect(capabilities).toMatchObject({
      width: 170,
      height: 320,
      deviceName: 'Test display',
      firmwareVersion: '1.0.0',
    });

    await transport.disconnect();
    expect(transport.connected).toBe(false);
  });

  it('requires a host', async () => {
    const transport = new WifiTransport({ WebSocketImpl: createFakeWebSocketImpl() });
    await expect(transport.connect({})).rejects.toThrow(/host/);
  });

  it('opens with the empty message the hotspot accepts', async () => {
    const WebSocketImpl = createFakeWebSocketImpl();
    const transport = new WifiTransport({ WebSocketImpl });
    await transport.connect({ host: '192.168.4.1' });

    expect(WebSocketImpl.sent[0]).toEqual(new Uint8Array());
    await transport.disconnect();
  });

  it('rejects a request when no response arrives', async () => {
    const transport = new WifiTransport({ WebSocketImpl: createFakeWebSocketImpl() });
    await transport.connect({ host: '192.168.4.1' });

    await expect(
      transport.request(Command.GET_STATUS, new Uint8Array(), { timeoutMs: 5 }),
    ).rejects.toThrow(/timed out/);
  });
});
