import { describe, expect, it, vi } from 'vitest';
import {
  Command,
  PacketDecoder,
  PROTOCOL_VERSION,
  WifiState,
  createBeginFramePayload,
  createBeginPlaylistPayload,
  createWifiCredentialsPayload,
  crc16Ccitt,
  encodePacket,
  parseHelloResponse,
  parseWifiStatusResponse,
} from './protocol.js';

describe('protocol framing', () => {
  it('uses the CCITT-FALSE check value', () => {
    expect(crc16Ccitt(new TextEncoder().encode('123456789'))).toBe(0x29b1);
  });

  it('round trips packets split across chunks', () => {
    const packet = encodePacket(Command.FRAME_CHUNK, 42, Uint8Array.of(1, 2, 3, 4));
    const decoder = new PacketDecoder();

    expect(decoder.push(packet.slice(0, 3))).toEqual([]);
    expect(decoder.push(packet.slice(3, 9))).toEqual([]);
    expect(decoder.push(packet.slice(9))).toEqual([{
      version: PROTOCOL_VERSION,
      command: Command.FRAME_CHUNK,
      sequence: 42,
      payload: Uint8Array.of(1, 2, 3, 4),
    }]);
  });

  it('reports corrupt packets and resynchronizes', () => {
    const onError = vi.fn();
    const bad = encodePacket(Command.HELLO, 1);
    bad[bad.length - 1] ^= 0xff;
    const good = encodePacket(Command.GET_STATUS, 2);
    const combined = new Uint8Array(bad.length + good.length);
    combined.set(bad);
    combined.set(good, bad.length);

    const packets = new PacketDecoder({ onError }).push(combined);
    expect(onError).toHaveBeenCalledOnce();
    expect(packets).toHaveLength(1);
    expect(packets[0].sequence).toBe(2);
  });
});

describe('capability response', () => {
  it('parses the display and firmware fields', () => {
    const name = new TextEncoder().encode('Cyberclip ST7789');
    const payload = new Uint8Array(20 + name.length);
    const view = new DataView(payload.buffer);
    view.setUint16(0, 170, true);
    view.setUint16(2, 320, true);
    view.setUint16(4, 4096, true);
    view.setUint32(6, 131072, true);
    view.setUint8(10, 0x0f);
    view.setUint8(11, 0x01);
    view.setUint8(12, 2);
    view.setUint8(13, 1);
    view.setUint8(14, 3);
    view.setUint8(15, 1);
    view.setUint32(16, 8 * 1024 * 1024, true);
    payload.set(name, 20);

    expect(parseHelloResponse(payload)).toMatchObject({
      width: 170,
      height: 320,
      maxChunk: 4096,
      maxFrameSize: 131072,
      firmwareVersion: '2.1.3',
      deviceName: 'Cyberclip ST7789',
      codecs: { jpeg: true },
      persistentStorage: true,
      maxStoredBytes: 8 * 1024 * 1024,
    });
  });
});

describe('persistent media payloads', () => {
  it('adds frame metadata only for persistent transfers', () => {
    const base = {
      transferId: 7,
      width: 170,
      height: 320,
      rotation: 0,
      totalSize: 12345,
    };
    expect(createBeginFramePayload(base)).toHaveLength(12);

    const persistent = createBeginFramePayload({
      ...base,
      frameIndex: 3,
      delayMs: 75,
    });
    expect(persistent).toHaveLength(16);
    const view = new DataView(persistent.buffer);
    expect(view.getUint16(12, true)).toBe(3);
    expect(view.getUint16(14, true)).toBe(75);
  });

  it('preserves GIF delays down to the source format resolution', () => {
    const payload = createBeginFramePayload({
      transferId: 7,
      width: 170,
      height: 320,
      rotation: 0,
      totalSize: 12345,
      frameIndex: 0,
      delayMs: 10,
    });

    expect(new DataView(payload.buffer).getUint16(14, true)).toBe(10);
  });

  it('encodes playlist metadata', () => {
    expect(createBeginPlaylistPayload({
      mediaType: 2,
      frameCount: 105,
      loop: true,
      rotation: 1,
    })).toEqual(Uint8Array.of(2, 105, 0, 1, 1));
  });
});

describe('WiFi payloads', () => {
  it('length-prefixes the SSID and password', () => {
    const payload = createWifiCredentialsPayload({ ssid: 'Home', password: 'sw0rdfish' });
    expect(payload).toEqual(Uint8Array.of(
      4, ...new TextEncoder().encode('Home'),
      9, ...new TextEncoder().encode('sw0rdfish'),
    ));
  });

  it('accepts an empty password for open networks', () => {
    expect(createWifiCredentialsPayload({ ssid: 'Home', password: '' })).toEqual(
      Uint8Array.of(4, ...new TextEncoder().encode('Home'), 0),
    );
  });

  it('rejects an empty or oversized SSID', () => {
    expect(() => createWifiCredentialsPayload({ ssid: '', password: '' })).toThrow();
    expect(() => createWifiCredentialsPayload({ ssid: 'x'.repeat(33), password: '' })).toThrow();
  });

  it('parses connection state, IP, hostname, and an optional token', () => {
    const hostname = new TextEncoder().encode('cyberclip');
    const token = Uint8Array.from({ length: 16 }, (_, i) => i);
    const payload = new Uint8Array([
      WifiState.CONNECTED, 192, 168, 1, 42,
      hostname.length, ...hostname,
      1, ...token,
    ]);

    expect(parseWifiStatusResponse(payload)).toMatchObject({
      state: WifiState.CONNECTED,
      connected: true,
      ip: '192.168.1.42',
      hostname: 'cyberclip',
    });
    expect(parseWifiStatusResponse(payload).token).toEqual(token);
  });

  it('omits the token when the device did not include one', () => {
    const hostname = new TextEncoder().encode('cyberclip');
    const payload = new Uint8Array([
      WifiState.CONNECTING, 0, 0, 0, 0,
      hostname.length, ...hostname,
      0,
    ]);

    expect(parseWifiStatusResponse(payload)).toMatchObject({
      state: WifiState.CONNECTING,
      connected: false,
      ip: '0.0.0.0',
      token: null,
    });
  });
});
