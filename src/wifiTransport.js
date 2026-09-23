import {
  Command,
  PacketDecoder,
  PROTOCOL_VERSION,
  ProtocolError,
  WIFI_TOKEN_SIZE,
  encodePacket,
  parseHelloResponse,
  parseNack,
} from './protocol.js';

const REQUEST_TIMEOUT_MS = 8000;

// WifiTransport mirrors SerialTransport's connect/request/disconnect shape
// so app.js can use either transport interchangeably. It carries the same
// framed protocol packets as USB serial, over a WebSocket to the device's
// local IP - see firmware/src/ws_server.h. Unlike Web Serial, opening it
// needs no user gesture, so it can be attempted automatically for a
// remembered device.
export class WifiTransport {
  constructor({
    WebSocketImpl = globalThis.WebSocket,
    onStateChange = () => {},
    onProtocolError = () => {},
  } = {}) {
    this.WebSocketImpl = WebSocketImpl;
    this.onStateChange = onStateChange;
    this.onProtocolError = onProtocolError;
    this.socket = null;
    this.host = null;
    this.sequence = 0;
    this.pending = new Map();
    this.disconnecting = false;
    this.decoder = new PacketDecoder({ onError: onProtocolError });
  }

  get connected() {
    return Boolean(this.socket) && this.socket.readyState === this.WebSocketImpl.OPEN;
  }

  async connect({
    host,
    token,
    trustedDevicePage = false,
    timeoutMs = REQUEST_TIMEOUT_MS,
  } = {}) {
    if (!this.WebSocketImpl) {
      throw new Error('WebSocket is not available in this browser');
    }
    if (!host) throw new Error('A device IP address or hostname is required');
    if (!trustedDevicePage && (!token || token.byteLength !== WIFI_TOKEN_SIZE)) {
      throw new Error('A WiFi pairing token is required. Pair over USB first.');
    }
    if (this.socket) await this.disconnect();

    this.disconnecting = false;
    this.host = host;
    this.onStateChange('connecting');

    const socket = await this.openSocket(host);
    if (trustedDevicePage) {
      socket.send(new Uint8Array());
    } else {
      socket.send(token);
    }
    this.socket = socket;
    this.socket.addEventListener('message', (event) => this.handleMessage(event));
    this.socket.addEventListener('close', () => {
      if (!this.disconnecting) {
        void this.disconnect({ reason: new Error('The WiFi connection closed') });
      }
    });

    try {
      this.onStateChange('handshaking');
      const response = await this.request(Command.HELLO, new Uint8Array(), {
        expectedCommand: Command.HELLO_RESPONSE,
        timeoutMs,
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

  openSocket(host) {
    return new Promise((resolve, reject) => {
      const socket = new this.WebSocketImpl(`ws://${host}/ws`);
      socket.binaryType = 'arraybuffer';
      socket.addEventListener('open', () => resolve(socket), { once: true });
      socket.addEventListener('error', () => {
        reject(new Error('Could not open a WiFi connection to the display'));
      }, { once: true });
    });
  }

  async request(command, payload, {
    expectedCommand = Command.ACK,
    timeoutMs = REQUEST_TIMEOUT_MS,
  } = {}) {
    if (!this.connected) throw new Error('The display is not connected');

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
      this.socket.send(packet);
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

    const disconnectError = reason ?? new Error('Display disconnected');
    for (const sequence of [...this.pending.keys()]) {
      this.rejectPending(sequence, disconnectError);
    }

    const socket = this.socket;
    this.socket = null;
    if (socket && socket.readyState !== this.WebSocketImpl.CLOSED) {
      try {
        socket.close();
      } catch {
        // The socket may already be closing.
      }
    }

    this.decoder = new PacketDecoder({ onError: this.onProtocolError });
    this.disconnecting = false;
    this.onStateChange('disconnected', reason);
  }

  handleMessage(event) {
    for (const packet of this.decoder.push(event.data)) {
      this.handlePacket(packet);
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
}
