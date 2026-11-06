import {
  Command,
  ERROR_STORAGE_FULL,
  ERROR_UNSUPPORTED_COMMAND,
  MediaType,
  ProtocolError,
  createReadStoredFramePayload,
  createSlotPayload,
  parseStoredFrameResponse,
  parseStoredListResponse,
} from './protocol.js';

export async function listStoredMedia(transport) {
  const response = await transport.request(Command.LIST_STORED, new Uint8Array(), {
    expectedCommand: Command.STORED_LIST_RESPONSE,
  });
  return parseStoredListResponse(response.payload);
}

// Reads one stored JPEG frame in chunks. Returns null when shouldContinue()
// turns false part-way, so the caller can give the link back to an upload.
export async function readStoredFrame(transport, slot, {
  frameIndex = 0,
  shouldContinue = () => true,
} = {}) {
  let offset = 0;
  let bytes = null;
  do {
    if (!shouldContinue()) return null;
    const response = await transport.request(
      Command.READ_STORED_FRAME,
      createReadStoredFramePayload({ slot, frameIndex, offset }),
      { expectedCommand: Command.STORED_FRAME_RESPONSE },
    );
    const chunk = parseStoredFrameResponse(response.payload);
    if (chunk.slot !== slot || chunk.frameIndex !== frameIndex || chunk.offset !== offset) {
      throw new ProtocolError('Device returned the wrong part of a saved frame');
    }
    if (bytes === null) {
      if (chunk.totalSize === 0) throw new ProtocolError('Saved frame is empty');
      bytes = new Uint8Array(chunk.totalSize);
    } else if (chunk.totalSize !== bytes.byteLength) {
      throw new ProtocolError('Saved frame changed while it was being read');
    }
    if (chunk.data.byteLength === 0 || offset + chunk.data.byteLength > bytes.byteLength) {
      throw new ProtocolError('Device sent a saved frame with the wrong length');
    }
    bytes.set(chunk.data, offset);
    offset += chunk.data.byteLength;
  } while (offset < bytes.byteLength);
  return bytes;
}

export function formatStorageBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export function describeStoredItem(item) {
  if (item.mediaType === MediaType.GIF) {
    return `GIF · ${item.frameCount} frame${item.frameCount === 1 ? '' : 's'}`;
  }
  return 'Image';
}

export function describeStorageUsage(library) {
  const used = formatStorageBytes(library.usedBytes);
  const total = formatStorageBytes(library.totalBytes);
  return `${used} of ${total} used`;
}

// Extra advice for an upload that failed; null when there is none.
export function uploadErrorHint(error) {
  if (error?.errorCode === ERROR_STORAGE_FULL) {
    return 'The device is full. Delete saved items under Saved media, then try again.';
  }
  return null;
}

// The "Saved media" panel shared by the desktop app and the hotspot page.
export class StoredMediaView {
  constructor({
    elements,
    getTransport,
    canUseDevice,
    log,
    confirm = (message) => globalThis.confirm(message),
  }) {
    this.elements = elements;
    this.getTransport = getTransport;
    this.canUseDevice = canUseDevice;
    this.log = log;
    this.confirm = confirm;
    this.library = null;
    this.thumbnails = new Map();
    this.working = false;
    this.refreshing = null;
    this.thumbnailRun = 0;

    elements.refreshButton.addEventListener('click', () => { void this.refresh(); });
    elements.list.addEventListener('click', (event) => {
      const button = event.target.closest('button[data-action]');
      if (!button || button.disabled) return;
      const slot = Number(button.closest('[data-slot]').dataset.slot);
      if (button.dataset.action === 'show') void this.show(slot);
      if (button.dataset.action === 'delete') void this.remove(slot);
    });
    this.reset();
  }

  // Disconnected: nothing to show.
  reset(message = 'Connect to see the media saved on the device.') {
    this.thumbnailRun += 1;
    this.library = null;
    this.setMessage(message);
    this.elements.list.replaceChildren();
    this.elements.usageLabel.textContent = 'Not connected';
    this.elements.count.textContent = '';
    this.elements.usageBar.value = 0;
    this.updateButtons();
  }

  async refresh() {
    if (this.refreshing) return this.refreshing;
    this.refreshing = this.loadLibrary().finally(() => {
      this.refreshing = null;
      this.updateButtons();
    });
    return this.refreshing;
  }

  async loadLibrary() {
    const transport = this.getTransport();
    if (!transport?.connected || !this.canUseDevice()) return;
    try {
      this.library = await listStoredMedia(transport);
    } catch (error) {
      if (error.errorCode === ERROR_UNSUPPORTED_COMMAND) {
        this.reset('Install the latest firmware to keep several items on the device.');
        this.elements.usageLabel.textContent = 'Update needed';
      } else {
        this.log(`Could not read saved media: ${error.message}`, 'error');
      }
      return;
    }
    this.render();
    void this.loadThumbnails();
  }

  render() {
    const { library, elements } = this;
    const { items } = library;
    elements.usageLabel.textContent = describeStorageUsage(library);
    elements.count.textContent = `${items.length} of ${library.maxItems} items`;
    elements.usageBar.value = library.totalBytes
      ? Math.round((library.usedBytes / library.totalBytes) * 100)
      : 0;
    this.setMessage(items.length === 0
      ? 'Nothing is saved yet. Turn on "Keep media on device" and send an image or GIF.'
      : '');

    const liveIds = new Set(items.map((item) => item.itemId));
    for (const [itemId, url] of this.thumbnails) {
      if (!liveIds.has(itemId)) {
        URL.revokeObjectURL(url);
        this.thumbnails.delete(itemId);
      }
    }

    elements.list.replaceChildren(...items.map((item, index) => this.renderItem(item, index)));
    this.updateButtons();
  }

  renderItem(item, index) {
    const entry = document.createElement('li');
    entry.className = 'library-item';
    entry.dataset.slot = String(item.slot);
    entry.dataset.itemId = String(item.itemId);
    if (item.active) entry.dataset.active = '';

    const thumbnail = document.createElement('div');
    thumbnail.className = 'library-thumb';
    const url = this.thumbnails.get(item.itemId);
    if (url) {
      thumbnail.append(this.createThumbnailImage(url, index));
    } else {
      const placeholder = document.createElement('span');
      placeholder.textContent = item.mediaType === MediaType.GIF ? 'GIF' : 'IMG';
      thumbnail.append(placeholder);
    }

    const info = document.createElement('div');
    info.className = 'library-info';
    const title = document.createElement('strong');
    title.textContent = describeStoredItem(item);
    const size = document.createElement('span');
    size.textContent = formatStorageBytes(item.bytes);
    info.append(title, size);
    if (item.active) {
      const badge = document.createElement('span');
      badge.className = 'pill pill-online';
      badge.textContent = 'On screen';
      info.append(badge);
    }

    const actions = document.createElement('div');
    actions.className = 'library-actions';
    actions.append(
      this.createButton('show', item.active ? 'Showing' : 'Show', 'button-secondary', `Show saved item ${index + 1}`),
      this.createButton('delete', 'Delete', 'button-quiet', `Delete saved item ${index + 1}`),
    );

    entry.append(thumbnail, info, actions);
    return entry;
  }

  createButton(action, label, style, accessibleName) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `button ${style}`;
    button.dataset.action = action;
    button.textContent = label;
    button.setAttribute('aria-label', accessibleName);
    return button;
  }

  createThumbnailImage(url, index) {
    const image = document.createElement('img');
    image.src = url;
    image.alt = `Saved item ${index + 1}`;
    return image;
  }

  // Fetches the first frame of each item without a thumbnail, one at a time,
  // and stops as soon as the device is needed for something else.
  async loadThumbnails() {
    const run = ++this.thumbnailRun;
    const transport = this.getTransport();
    const stillWanted = () => run === this.thumbnailRun
      && transport.connected && this.canUseDevice() && !this.working;
    for (const item of this.library?.items ?? []) {
      if (!stillWanted()) return;
      if (this.thumbnails.has(item.itemId)) continue;
      let bytes;
      try {
        bytes = await readStoredFrame(transport, item.slot, { shouldContinue: stillWanted });
      } catch (error) {
        if (stillWanted()) this.log(`Could not load a saved item preview: ${error.message}`, 'error');
        return;
      }
      if (!bytes) return;
      const url = URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' }));
      this.thumbnails.set(item.itemId, url);
      const entry = this.elements.list.querySelector(`[data-item-id="${item.itemId}"] .library-thumb`);
      if (entry) {
        const index = this.library.items.indexOf(item);
        entry.replaceChildren(this.createThumbnailImage(url, index));
      }
    }
  }

  async show(slot) {
    await this.runAction(async (transport) => {
      await transport.request(Command.SELECT_STORED, createSlotPayload(slot), { timeoutMs: 15000 });
      this.log('Showing saved item');
    }, 'Could not show saved item');
  }

  async remove(slot) {
    const item = this.library?.items.find((candidate) => candidate.slot === slot);
    const kind = item?.mediaType === MediaType.GIF ? 'GIF' : 'image';
    if (!this.confirm(`Delete this saved ${kind} from the device? This cannot be undone.`)) return;
    await this.runAction(async (transport) => {
      await transport.request(Command.DELETE_STORED, createSlotPayload(slot), { timeoutMs: 30000 });
      this.log(`Deleted saved ${kind}`);
    }, 'Could not delete saved item');
  }

  async runAction(action, failureMessage) {
    const transport = this.getTransport();
    if (!transport?.connected || !this.canUseDevice() || this.working) return;
    this.working = true;
    this.thumbnailRun += 1;
    this.updateButtons();
    try {
      await action(transport);
    } catch (error) {
      this.log(`${failureMessage}: ${error.message}`, 'error');
    } finally {
      this.working = false;
    }
    await this.refresh();
  }

  setMessage(message) {
    this.elements.empty.textContent = message;
    this.elements.empty.hidden = !message;
  }

  // Called by the page whenever its own state changes.
  updateButtons() {
    const transport = this.getTransport();
    const usable = Boolean(transport?.connected) && this.canUseDevice()
      && !this.working && !this.refreshing;
    this.elements.refreshButton.disabled = !usable || !transport?.connected;
    for (const button of this.elements.list.querySelectorAll('button[data-action]')) {
      const isShowing = button.dataset.action === 'show'
        && button.closest('[data-active]') !== null;
      button.disabled = !usable || isShowing;
    }
  }
}
