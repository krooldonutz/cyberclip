export const MAGIC = Uint8Array.of(0x43, 0x43);
export const PROTOCOL_VERSION = 2;
export const HEADER_SIZE = 10;
export const CRC_SIZE = 2;
export const MAX_WIRE_PAYLOAD = 4096;

export const Command = Object.freeze({
  HELLO: 0x01,
  BEGIN_FRAME: 0x10,
  FRAME_CHUNK: 0x11,
  COMMIT_FRAME: 0x12,
  CANCEL_TRANSFER: 0x13,
  SET_BACKLIGHT: 0x20,
  CLEAR_DISPLAY: 0x21,
  GET_STATUS: 0x22,
  BEGIN_PLAYLIST: 0x30,
  END_PLAYLIST: 0x31,
  PLAY_STORED: 0x32,
  CLEAR_STORED: 0x33,
  HELLO_RESPONSE: 0x81,
  STATUS_RESPONSE: 0xa2,
  ACK: 0xf0,
  NACK: 0xf1,
});

export const ErrorCode = Object.freeze({
  1: 'Unsupported protocol version',
  2: 'Unsupported command',
  3: 'Invalid payload',
  4: 'CRC validation failed',
  5: 'Device is busy',
  6: 'Value is out of range',
  7: 'Device does not have enough memory',
  8: 'JPEG decode failed',
  9: 'No transfer is active',
  10: 'Transfer sequence is invalid',
});

export const Codec = Object.freeze({
  JPEG: 1,
});

export class ProtocolError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = 'ProtocolError';
    Object.assign(this, details);
  }
}

export function crc16Ccitt(bytes) {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) !== 0
        ? ((crc << 1) ^ 0x1021) & 0xffff
        : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export function encodePacket(command, sequence, payload = new Uint8Array()) {
  const body = asUint8Array(payload);
  if (body.byteLength > MAX_WIRE_PAYLOAD) {
    throw new ProtocolError(`Payload exceeds ${MAX_WIRE_PAYLOAD} bytes`);
  }

  const packet = new Uint8Array(HEADER_SIZE + body.byteLength + CRC_SIZE);
  const view = new DataView(packet.buffer);
  packet.set(MAGIC, 0);
  view.setUint8(2, PROTOCOL_VERSION);
  view.setUint8(3, command);
  view.setUint16(4, sequence, true);
  view.setUint32(6, body.byteLength, true);
  packet.set(body, HEADER_SIZE);
  view.setUint16(
    HEADER_SIZE + body.byteLength,
    crc16Ccitt(packet.subarray(2, HEADER_SIZE + body.byteLength)),
    true,
  );
  return packet;
}

export class PacketDecoder {
  constructor({ maxPayload = MAX_WIRE_PAYLOAD, onError = () => {} } = {}) {
    this.maxPayload = maxPayload;
    this.onError = onError;
    this.buffer = new Uint8Array();
  }

  push(chunk) {
    const incoming = asUint8Array(chunk);
    const combined = new Uint8Array(this.buffer.byteLength + incoming.byteLength);
    combined.set(this.buffer);
    combined.set(incoming, this.buffer.byteLength);
    this.buffer = combined;

    const packets = [];
    while (this.buffer.byteLength >= 2) {
      const magicIndex = findMagic(this.buffer);
      if (magicIndex === -1) {
        this.buffer = this.buffer.at(-1) === MAGIC[0]
          ? this.buffer.slice(-1)
          : new Uint8Array();
        break;
      }
      if (magicIndex > 0) this.buffer = this.buffer.slice(magicIndex);
      if (this.buffer.byteLength < HEADER_SIZE) break;

      const view = new DataView(
        this.buffer.buffer,
        this.buffer.byteOffset,
        this.buffer.byteLength,
      );
      const payloadLength = view.getUint32(6, true);
      if (payloadLength > this.maxPayload) {
        this.onError(new ProtocolError(`Incoming payload exceeds ${this.maxPayload} bytes`));
        this.buffer = this.buffer.slice(2);
        continue;
      }

      const packetLength = HEADER_SIZE + payloadLength + CRC_SIZE;
      if (this.buffer.byteLength < packetLength) break;

      const candidate = this.buffer.slice(0, packetLength);
      this.buffer = this.buffer.slice(packetLength);
      const candidateView = new DataView(candidate.buffer);
      const expectedCrc = candidateView.getUint16(packetLength - CRC_SIZE, true);
      const actualCrc = crc16Ccitt(candidate.subarray(2, packetLength - CRC_SIZE));
      if (actualCrc !== expectedCrc) {
        this.onError(new ProtocolError('Received a packet with an invalid CRC'));
        continue;
      }

      packets.push({
        version: candidateView.getUint8(2),
        command: candidateView.getUint8(3),
        sequence: candidateView.getUint16(4, true),
        payload: candidate.slice(HEADER_SIZE, HEADER_SIZE + payloadLength),
      });
    }
    return packets;
  }
}

export function parseHelloResponse(payload) {
  if (payload.byteLength < 20) {
    throw new ProtocolError('Device returned an incomplete capability response');
  }
  const view = viewOf(payload);
  const codecMask = view.getUint8(11);
  return {
    width: view.getUint16(0, true),
    height: view.getUint16(2, true),
    maxChunk: view.getUint16(4, true),
    maxFrameSize: view.getUint32(6, true),
    rotationMask: view.getUint8(10),
    codecMask,
    codecs: {
      jpeg: (codecMask & 0x01) !== 0,
    },
    firmwareVersion: `${view.getUint8(12)}.${view.getUint8(13)}.${view.getUint8(14)}`,
    persistentStorage: view.getUint8(15) === 1,
    maxStoredBytes: view.getUint32(16, true),
    deviceName: new TextDecoder().decode(payload.subarray(20)) || 'Cyberclip display',
  };
}

export function parseNack(payload) {
  const requestCommand = payload.at(0);
  const errorCode = payload.at(1);
  return new ProtocolError(
    ErrorCode[errorCode] ?? `Device error ${errorCode ?? 'unknown'}`,
    { requestCommand, errorCode },
  );
}

export function createBeginFramePayload({
  transferId,
  width,
  height,
  codec = Codec.JPEG,
  rotation,
  totalSize,
  frameIndex,
  delayMs,
}) {
  const persistent = frameIndex !== undefined;
  const payload = new Uint8Array(persistent ? 16 : 12);
  const view = new DataView(payload.buffer);
  view.setUint16(0, transferId, true);
  view.setUint16(2, width, true);
  view.setUint16(4, height, true);
  view.setUint8(6, codec);
  view.setUint8(7, rotation);
  view.setUint32(8, totalSize, true);
  if (persistent) {
    view.setUint16(12, frameIndex, true);
    view.setUint16(14, Math.min(0xffff, Math.max(10, delayMs ?? 100)), true);
  }
  return payload;
}

export function createBeginPlaylistPayload({
  mediaType,
  frameCount,
  loop,
  rotation,
}) {
  const payload = new Uint8Array(5);
  const view = new DataView(payload.buffer);
  view.setUint8(0, mediaType);
  view.setUint16(1, frameCount, true);
  view.setUint8(3, loop ? 1 : 0);
  view.setUint8(4, rotation);
  return payload;
}

export function createFrameChunkPayload(transferId, offset, bytes) {
  const data = asUint8Array(bytes);
  if (data.byteLength + 6 > MAX_WIRE_PAYLOAD) {
    throw new ProtocolError('Frame chunk is too large');
  }
  const payload = new Uint8Array(6 + data.byteLength);
  const view = new DataView(payload.buffer);
  view.setUint16(0, transferId, true);
  view.setUint32(2, offset, true);
  payload.set(data, 6);
  return payload;
}

export function createTransferIdPayload(transferId) {
  const payload = new Uint8Array(2);
  new DataView(payload.buffer).setUint16(0, transferId, true);
  return payload;
}

export function createClearPayload(rgb565 = 0) {
  const payload = new Uint8Array(2);
  new DataView(payload.buffer).setUint16(0, rgb565, true);
  return payload;
}

function asUint8Array(value) {
  if (value instanceof Uint8Array) return value;
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  return Uint8Array.from(value);
}

function viewOf(bytes) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function findMagic(bytes) {
  for (let index = 0; index < bytes.byteLength - 1; index += 1) {
    if (bytes[index] === MAGIC[0] && bytes[index + 1] === MAGIC[1]) return index;
  }
  return -1;
}
