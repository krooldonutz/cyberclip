import {
  Command,
  PacketDecoder,
  PROTOCOL_VERSION,
  ProtocolError,
  encodePacket,
  parseHelloResponse,
  parseNack,
} from './protocol.js';

export const SERIAL_BAUD_RATE = 921600;
const REQUEST_TIMEOUT_MS = 5000;
const HANDSHAKE_TIMEOUT_MS = 20000;

export class SerialTransport {
  constructor({
    serial = globalThis.navigator?.serial,
    onStateChange = () => {},
    onProtocolError = () => {},
  } = {}) {
    this.serial = serial;
    this.onStateChange = onStateChange;
    this.onProtocolError = onProtocolError;
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.readTask = null;
    this.sequence = 0;
    this.pending = new Map();
    this.disconnecting = false;
    this.decoder = new PacketDecoder({ onError: onProtocolError });
    this.handlePhysicalDisconnect = this.handlePhysicalDisconnect.bind(this);
  }

  get connected() {
    return Boolean(this.port?.readable && this.port?.writable);
  }

  async connect({ port, filters } = {}) {
    if (!this.serial) {
      throw new Error('Web Serial is not available in this browser');
    }
    if (this.port) await this.disconnect();

    this.disconnecting = false;
    this.onStateChange('connecting');
    this.port = port ?? await this.serial.requestPort(filters?.length ? { filters } : undefined);

    try {
      await this.port.open({
        baudRate: SERIAL_BAUD_RATE,
        bufferSize: 16384,
        dataBits: 8,
        stopBits: 1,
        parity: 'none',
        flowControl: 'none',
      });
      this.writer = this.port.writable.getWriter();
      this.reader = this.port.readable.getReader();
      this.serial.addEventListener?.('disconnect', this.handlePhysicalDisconnect);
      this.readTask = this.readLoop();
      await delay(1000);
      this.onStateChange('handshaking');

      const response = await this.request(Command.HELLO, new Uint8Array(), {
        expectedCommand: Command.HELLO_RESPONSE,
        timeoutMs: HANDSHAKE_TIMEOUT_MS,
      });
      if (response.version !== PROTOCOL_VERSION) {
        throw new ProtocolError(`Unsupported firmware protocol version ${response.version}`);
      }

      const capabilities = parseHelloResponse(response.payload);
      if (!capabilities.codecs.jpeg) {
        throw new ProtocolError('The connected firmware does not support JPEG frames');
      }
      this.onStateChange('ready', capabilities);
      return capabilities;
    } catch (error) {
      await this.disconnect({ reason: error });
      throw error;
    }
  }

  async reconnectAuthorized() {
    if (!this.serial) return null;
    const [port] = await this.serial.getPorts();
    if (!port) return null;
    return this.connect({ port });
  }

  async request(command, payload, {
    expectedCommand = Command.ACK,
    timeoutMs = REQUEST_TIMEOUT_MS,
  } = {}) {
    if (!this.writer) throw new Error('The display is not connected');

    this.sequence = (this.sequence + 1) & 0xffff;
    if (this.sequence === 0) this.sequence = 1;
    const sequence = this.sequence;
    const packet = encodePacket(command, sequence, payload);

    const responsePromise = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(sequence);
        reject(new ProtocolError(`Device timed out while handling command 0x${command.toString(16)}`));
      }, timeoutMs);
      this.pending.set(sequence, { command, expectedCommand, resolve, reject, timeout });
    });

    try {
      await this.writer.write(packet);
    } catch (error) {
      this.rejectPending(sequence, error);
      throw error;
    }
    return responsePromise;
  }

  async disconnect({ reason } = {}) {
    if (this.disconnecting) return;
    this.disconnecting = true;
    this.onStateChange('disconnecting');
    this.serial?.removeEventListener?.('disconnect', this.handlePhysicalDisconnect);

    const disconnectError = reason ?? new Error('Display disconnected');
    for (const sequence of [...this.pending.keys()]) {
      this.rejectPending(sequence, disconnectError);
    }

    try {
      await this.reader?.cancel();
    } catch {
      // The stream may already be closed after a physical disconnect.
    }
    try {
      await this.readTask;
    } catch {
      // readLoop reports unexpected failures through the state callback.
    }
    this.reader?.releaseLock();
    this.reader = null;
    this.readTask = null;

    try {
      await this.writer?.close();
    } catch {
      // The writable stream may already be unavailable.
    }
    this.writer?.releaseLock();
    this.writer = null;

    try {
      await this.port?.close();
    } catch {
      // Closing an already disconnected USB port is harmless.
    }
    this.port = null;
    this.decoder = new PacketDecoder({ onError: this.onProtocolError });
    this.disconnecting = false;
    this.onStateChange('disconnected', reason);
  }

  async readLoop() {
    try {
      while (this.reader) {
        const { value, done } = await this.reader.read();
        if (done) break;
        for (const packet of this.decoder.push(value)) {
          this.handlePacket(packet);
        }
      }
      if (!this.disconnecting) {
        queueMicrotask(() => this.disconnect({ reason: new Error('The USB connection closed') }));
      }
    } catch (error) {
      if (!this.disconnecting) {
        this.onProtocolError(error);
        queueMicrotask(() => this.disconnect({ reason: error }));
      }
    }
  }

  handlePacket(packet) {
    const pending = this.pending.get(packet.sequence);
    if (!pending) {
      this.onProtocolError(new ProtocolError(`Unexpected response sequence ${packet.sequence}`));
      return;
    }
    clearTimeout(pending.timeout);
    this.pending.delete(packet.sequence);

    if (packet.version !== PROTOCOL_VERSION) {
      pending.reject(new ProtocolError(
        `Device responded with protocol version ${packet.version}`,
      ));
      return;
    }
    if (packet.command === Command.NACK) {
      pending.reject(parseNack(packet.payload));
      return;
    }
    if (packet.command !== pending.expectedCommand) {
      pending.reject(new ProtocolError(
        `Expected response 0x${pending.expectedCommand.toString(16)}, received 0x${packet.command.toString(16)}`,
      ));
      return;
    }
    if (packet.command === Command.ACK && packet.payload.at(0) !== pending.command) {
      pending.reject(new ProtocolError('ACK did not match the request command'));
      return;
    }
    pending.resolve(packet);
  }

  rejectPending(sequence, error) {
    const pending = this.pending.get(sequence);
    if (!pending) return;
    clearTimeout(pending.timeout);
    this.pending.delete(sequence);
    pending.reject(error);
  }

  handlePhysicalDisconnect(event) {
    if (!this.port || (event.target && event.target !== this.port)) return;
    void this.disconnect({ reason: new Error('The USB cable was disconnected') });
  }
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
