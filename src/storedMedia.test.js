import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  Command,
  ERROR_STORAGE_FULL,
  MAX_STORED_FRAME_READ,
  MediaType,
  NO_STORED_SLOT,
  ProtocolError,
  createReadStoredFramePayload,
  createSlotPayload,
  parseStoredFrameResponse,
  parseStoredListResponse,
} from './protocol.js';
import {
  StoredMediaView,
  describeStoredItem,
  formatStorageBytes,
  readStoredFrame,
  uploadErrorHint,
} from './storedMedia.js';

function encodeList({ totalBytes, usedBytes, maxItems = 16, activeSlot, items }) {
  const payload = new Uint8Array(11 + items.length * 14);
  const view = new DataView(payload.buffer);
  view.setUint32(0, totalBytes, true);
  view.setUint32(4, usedBytes, true);
  view.setUint8(8, maxItems);
  view.setUint8(9, activeSlot ?? NO_STORED_SLOT);
  view.setUint8(10, items.length);
  items.forEach((item, index) => {
    const offset = 11 + index * 14;
    view.setUint8(offset, item.slot);
    view.setUint8(offset + 1, item.mediaType);
    view.setUint16(offset + 2, item.frameCount, true);
    view.setUint8(offset + 4, item.rotation ?? 0);
    view.setUint8(offset + 5, item.loop ? 1 : 0);
    view.setUint32(offset + 6, item.bytes, true);
    view.setUint32(offset + 10, item.itemId, true);
  });
  return payload;
}

// Answers READ_STORED_FRAME like the firmware does, from in-memory frames.
function frameReader(frames) {
  return (payload) => {
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const slot = view.getUint8(0);
    const offset = view.getUint32(3, true);
    const maxLength = view.getUint16(7, true);
    const frame = frames[slot];
    const data = frame.subarray(offset, offset + Math.min(maxLength, MAX_STORED_FRAME_READ));
    const reply = new Uint8Array(11 + data.byteLength);
    const replyView = new DataView(reply.buffer);
    replyView.setUint8(0, slot);
    replyView.setUint16(1, view.getUint16(1, true), true);
    replyView.setUint32(3, frame.byteLength, true);
    replyView.setUint32(7, offset, true);
    reply.set(data, 11);
    return reply;
  };
}

function createTransport(handlers) {
  const calls = [];
  return {
    connected: true,
    calls,
    async request(command, payload, options = {}) {
      calls.push({ command, payload, options });
      const handler = handlers[command];
      if (!handler) return { payload: Uint8Array.of(command) };
      const result = await handler(payload);
      if (result instanceof Error) throw result;
      return { payload: result ?? Uint8Array.of(command) };
    },
  };
}

describe('saved media protocol', () => {
  it('parses the saved media list', () => {
    const library = parseStoredListResponse(encodeList({
      totalBytes: 9_000_000,
      usedBytes: 1_250_000,
      activeSlot: 3,
      items: [
        { slot: 0, mediaType: MediaType.IMAGE, frameCount: 1, bytes: 40_000, itemId: 7 },
        { slot: 3, mediaType: MediaType.GIF, frameCount: 24, loop: true, rotation: 1, bytes: 900_000, itemId: 0xdeadbeef },
      ],
    }));

    expect(library).toMatchObject({
      totalBytes: 9_000_000,
      usedBytes: 1_250_000,
      maxItems: 16,
      activeSlot: 3,
    });
    expect(library.items).toEqual([
      { slot: 0, mediaType: 1, frameCount: 1, rotation: 0, loop: false, bytes: 40_000, itemId: 7, active: false },
      { slot: 3, mediaType: 2, frameCount: 24, rotation: 1, loop: true, bytes: 900_000, itemId: 0xdeadbeef, active: true },
    ]);
  });

  it('reports no active item and rejects a list with the wrong length', () => {
    const empty = parseStoredListResponse(encodeList({ totalBytes: 1, usedBytes: 0, items: [] }));
    expect(empty.activeSlot).toBeNull();
    expect(empty.items).toEqual([]);

    const truncated = encodeList({
      totalBytes: 1,
      usedBytes: 0,
      items: [{ slot: 0, mediaType: 1, frameCount: 1, bytes: 1, itemId: 1 }],
    }).slice(0, -1);
    expect(() => parseStoredListResponse(truncated)).toThrow(ProtocolError);
  });

  it('encodes slot and frame read requests', () => {
    expect(createSlotPayload(15)).toEqual(Uint8Array.of(15));
    expect(() => createSlotPayload(NO_STORED_SLOT)).toThrow(ProtocolError);
    expect(() => createSlotPayload(-1)).toThrow(ProtocolError);

    const payload = createReadStoredFramePayload({ slot: 2, frameIndex: 300, offset: 70_000, maxLength: 99_999 });
    const view = new DataView(payload.buffer);
    expect(payload.byteLength).toBe(9);
    expect(view.getUint8(0)).toBe(2);
    expect(view.getUint16(1, true)).toBe(300);
    expect(view.getUint32(3, true)).toBe(70_000);
    expect(view.getUint16(7, true)).toBe(MAX_STORED_FRAME_READ);
  });

  it('parses a saved frame chunk', () => {
    const reply = frameReader({ 4: Uint8Array.of(9, 8, 7, 6) })(createReadStoredFramePayload({ slot: 4, offset: 1 }));
    expect(parseStoredFrameResponse(reply)).toEqual({
      slot: 4,
      frameIndex: 0,
      totalSize: 4,
      offset: 1,
      data: Uint8Array.of(8, 7, 6),
    });
  });
});

describe('readStoredFrame', () => {
  it('reads a frame larger than one reply', async () => {
    const frame = Uint8Array.from({ length: 10_000 }, (_, index) => index % 251);
    const transport = createTransport({ [Command.READ_STORED_FRAME]: frameReader({ 5: frame }) });

    const bytes = await readStoredFrame(transport, 5);

    expect(bytes).toEqual(frame);
    expect(transport.calls).toHaveLength(3);
    expect(transport.calls[0].options.expectedCommand).toBe(Command.STORED_FRAME_RESPONSE);
  });

  it('stops when the device is needed for something else', async () => {
    const frame = new Uint8Array(10_000);
    const transport = createTransport({ [Command.READ_STORED_FRAME]: frameReader({ 1: frame }) });
    let allowed = 1;

    const bytes = await readStoredFrame(transport, 1, { shouldContinue: () => allowed-- > 0 });

    expect(bytes).toBeNull();
    expect(transport.calls).toHaveLength(1);
  });

  it('rejects a reply for a different part of the frame', async () => {
    const reader = frameReader({ 1: new Uint8Array(10_000) });
    const transport = createTransport({
      [Command.READ_STORED_FRAME]: (payload) => {
        const reply = reader(payload);
        new DataView(reply.buffer).setUint32(7, 123, true);
        return reply;
      },
    });

    await expect(readStoredFrame(transport, 1)).rejects.toThrow('wrong part');
  });
});

describe('saved media text', () => {
  it('describes items, sizes and upload failures', () => {
    expect(describeStoredItem({ mediaType: MediaType.GIF, frameCount: 1 })).toBe('GIF · 1 frame');
    expect(describeStoredItem({ mediaType: MediaType.GIF, frameCount: 12 })).toBe('GIF · 12 frames');
    expect(describeStoredItem({ mediaType: MediaType.IMAGE, frameCount: 1 })).toBe('Image');
    expect(formatStorageBytes(512)).toBe('512 B');
    expect(formatStorageBytes(40_960)).toBe('40 KB');
    expect(formatStorageBytes(9 * 1024 * 1024)).toBe('9.0 MB');
    expect(uploadErrorHint(new ProtocolError('full', { errorCode: ERROR_STORAGE_FULL }))).toMatch(/Delete saved items/);
    expect(uploadErrorHint(new Error('other'))).toBeNull();
  });
});

describe('StoredMediaView', () => {
  let elements;
  let urlCount;

  beforeEach(() => {
    document.body.innerHTML = `
      <button id="refresh"></button>
      <span id="usage-label"></span><span id="count"></span>
      <progress id="usage" max="100"></progress>
      <p id="empty"></p>
      <ul id="list"></ul>`;
    elements = {
      refreshButton: document.getElementById('refresh'),
      usageLabel: document.getElementById('usage-label'),
      count: document.getElementById('count'),
      usageBar: document.getElementById('usage'),
      empty: document.getElementById('empty'),
      list: document.getElementById('list'),
    };
    urlCount = 0;
    URL.createObjectURL = vi.fn(() => `blob:thumb-${(urlCount += 1)}`);
    URL.revokeObjectURL = vi.fn();
  });

  afterEach(() => {
    delete URL.createObjectURL;
    delete URL.revokeObjectURL;
  });

  function libraryTransport(state) {
    const frames = { 0: Uint8Array.of(1, 2, 3), 2: Uint8Array.of(4, 5, 6) };
    return createTransport({
      [Command.LIST_STORED]: () => encodeList(state),
      [Command.READ_STORED_FRAME]: frameReader(frames),
      [Command.SELECT_STORED]: (payload) => { state.activeSlot = payload[0]; },
      [Command.DELETE_STORED]: (payload) => {
        state.items = state.items.filter((item) => item.slot !== payload[0]);
      },
    });
  }

  function createView(transport, options = {}) {
    return new StoredMediaView({
      elements,
      getTransport: () => transport,
      canUseDevice: () => true,
      log: vi.fn(),
      ...options,
    });
  }

  const twoItems = () => ({
    totalBytes: 8 * 1024 * 1024,
    usedBytes: 2 * 1024 * 1024,
    activeSlot: 2,
    items: [
      { slot: 0, mediaType: MediaType.IMAGE, frameCount: 1, bytes: 30_000, itemId: 11 },
      { slot: 2, mediaType: MediaType.GIF, frameCount: 8, bytes: 400_000, itemId: 22 },
    ],
  });

  it('lists saved items with usage, the item on screen, and thumbnails', async () => {
    const transport = libraryTransport(twoItems());
    const view = createView(transport);

    await view.refresh();
    await vi.waitFor(() => expect(elements.list.querySelectorAll('img')).toHaveLength(2));

    expect(elements.usageLabel.textContent).toBe('2.0 MB of 8.0 MB used');
    expect(elements.count.textContent).toBe('2 of 16 items');
    expect(elements.usageBar.value).toBe(25);
    expect(elements.empty.hidden).toBe(true);
    const entries = elements.list.querySelectorAll('.library-item');
    expect(entries[0].textContent).toContain('Image');
    expect(entries[1].textContent).toContain('GIF · 8 frames');
    expect(entries[1].textContent).toContain('On screen');
    expect(entries[1].querySelector('[data-action="show"]').disabled).toBe(true);
    expect(entries[0].querySelector('[data-action="show"]').disabled).toBe(false);
  });

  it('shows another item and deletes after confirmation', async () => {
    const state = twoItems();
    const transport = libraryTransport(state);
    const askToConfirm = vi.fn(async () => true);
    const view = createView(transport, { askToConfirm });
    await view.refresh();

    await view.show(0);
    expect(transport.calls.some((call) => call.command === Command.SELECT_STORED
      && call.payload[0] === 0)).toBe(true);
    expect(elements.list.querySelector('[data-slot="0"]').hasAttribute('data-active')).toBe(true);

    await view.remove(2);
    expect(askToConfirm).toHaveBeenCalledWith(expect.objectContaining({
      title: expect.stringContaining('saved GIF'),
      destructive: true,
    }));
    expect(elements.list.querySelectorAll('.library-item')).toHaveLength(1);
  });

  it('does not delete when the confirmation is declined', async () => {
    const transport = libraryTransport(twoItems());
    const view = createView(transport, { askToConfirm: async () => false });
    await view.refresh();

    await view.remove(0);

    expect(transport.calls.some((call) => call.command === Command.DELETE_STORED)).toBe(false);
  });

  it('explains an empty library and older firmware', async () => {
    const empty = createView(libraryTransport({ totalBytes: 100, usedBytes: 0, items: [] }));
    await empty.refresh();
    expect(elements.empty.textContent).toMatch(/Nothing is saved yet/);

    const old = createView(createTransport({
      [Command.LIST_STORED]: () => new ProtocolError('Unsupported command', { errorCode: 2 }),
    }));
    await old.refresh();
    expect(elements.empty.textContent).toMatch(/latest firmware/);
    expect(elements.list.children).toHaveLength(0);
  });

  it('leaves the device alone while it is busy', async () => {
    const transport = libraryTransport(twoItems());
    const view = createView(transport, { canUseDevice: () => false });

    await view.refresh();

    expect(transport.calls).toHaveLength(0);
    expect(elements.refreshButton.disabled).toBe(true);
  });
});
