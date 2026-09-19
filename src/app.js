import './styles.css';
import { decodeGif, iterateGifFrames } from './gifs.js';
import { encodeSourceForDisplay, prepareImageFile } from './images.js';
import {
  Codec,
  Command,
  createBeginFramePayload,
  createBeginPlaylistPayload,
  createClearPayload,
  createFrameChunkPayload,
  createTransferIdPayload,
} from './protocol.js';
import { SerialTransport } from './serial.js';

const DEFAULT_CAPABILITIES = {
  width: 170,
  height: 320,
  maxChunk: 4096,
  maxFrameSize: 128 * 1024,
  rotationMask: 0x0f,
  codecs: { jpeg: true },
};

const elements = Object.fromEntries([
  'connection-pill',
  'install-button',
  'connect-button',
  'disconnect-button',
  'support-message',
  'device-resolution',
  'device-firmware',
  'device-codec',
  'device-storage',
  'media-badge',
  'preview-image',
  'preview-empty',
  'media-input',
  'display-button',
  'play-button',
  'stop-button',
  'clear-button',
  'remove-stored-button',
  'transfer-progress',
  'progress-label',
  'progress-value',
  'fit-select',
  'rotation-select',
  'background-input',
  'quality-input',
  'quality-output',
  'backlight-input',
  'backlight-output',
  'gif-delay-input',
  'loop-input',
  'persist-input',
  'clear-log-button',
  'activity-log',
  'log-template',
].map((id) => [id, document.getElementById(id)]));

let state = 'disconnected';
let capabilities = DEFAULT_CAPABILITIES;
let selectedFile = null;
let decodedGif = null;
let previewUrl = null;
let transferId = 0;
let activeTransferId = null;
let playbackController = null;
let installPrompt = null;
const webSerialAvailable = globalThis.isSecureContext && 'serial' in navigator;

const transport = new SerialTransport({
  onStateChange(nextState, details) {
    if (nextState === 'ready') {
      capabilities = details;
      updateDeviceDetails();
      log(`Connected to ${details.deviceName} with firmware ${details.firmwareVersion}`);
    }
    if (
      nextState === 'disconnected'
      && details
      && !['connecting', 'handshaking', 'disconnecting'].includes(state)
    ) {
      log(details.message, 'error');
    }
    if (['connecting', 'handshaking', 'ready', 'disconnecting', 'disconnected'].includes(nextState)) {
      setState(nextState);
    }
  },
  onProtocolError(error) {
    log(error.message, 'error');
  },
});

initialize();

function initialize() {
  restorePreferences();
  updateRangeOutputs();
  setState('disconnected');
  updateSupportMessage();
  setProgress(0, 'Ready');
  bindEvents();

  if (import.meta.env.PROD && 'serviceWorker' in navigator && globalThis.isSecureContext) {
    navigator.serviceWorker.register('/service-worker.js').catch((error) => {
      log(`Offline support could not start: ${error.message}`, 'error');
    });
  } else if ('serviceWorker' in navigator) {
    void navigator.serviceWorker.getRegistrations()
      .then((registrations) => Promise.all(
        registrations.map((registration) => registration.unregister()),
      ));
  }
}

function bindEvents() {
  elements['connect-button'].addEventListener('click', connect);
  elements['disconnect-button'].addEventListener('click', () => transport.disconnect());
  elements['media-input'].addEventListener('change', handleMediaSelection);
  elements['display-button'].addEventListener('click', displayImage);
  elements['play-button'].addEventListener('click', playGif);
  elements['stop-button'].addEventListener('click', stopPlayback);
  elements['clear-button'].addEventListener('click', clearDisplay);
  elements['remove-stored-button'].addEventListener('click', removeStoredMedia);
  elements['clear-log-button'].addEventListener('click', () => {
    elements['activity-log'].replaceChildren();
  });

  for (const id of ['fit-select', 'rotation-select', 'background-input', 'quality-input']) {
    elements[id].addEventListener('change', () => {
      savePreferences();
      void refreshPreview();
    });
  }
  elements['quality-input'].addEventListener('input', updateRangeOutputs);
  elements['backlight-input'].addEventListener('input', updateRangeOutputs);
  elements['backlight-input'].addEventListener('change', updateBacklight);
  elements['gif-delay-input'].addEventListener('change', savePreferences);
  elements['loop-input'].addEventListener('change', savePreferences);
  elements['persist-input'].addEventListener('change', savePreferences);

  window.addEventListener('beforeinstallprompt', (event) => {
    event.preventDefault();
    installPrompt = event;
    elements['install-button'].hidden = false;
  });
  elements['install-button'].addEventListener('click', async () => {
    await installPrompt?.prompt();
    installPrompt = null;
    elements['install-button'].hidden = true;
  });
}

async function connect() {
  let lastError;
  try {
    const authorizedPorts = await navigator.serial.getPorts();
    for (const port of authorizedPorts) {
      try {
        await transport.connect({ port });
        return;
      } catch (error) {
        lastError = error;
      }
    }
    if (authorizedPorts.length === 0) {
      await transport.connect();
      return;
    }
    throw lastError;
  } catch (error) {
    const reportedError = error.name === 'NotFoundError' ? lastError : error;
    if (reportedError) log(`Connection failed: ${reportedError.message}`, 'error');
    setState('disconnected');
  }
}

async function handleMediaSelection(event) {
  stopPlayback();
  selectedFile = event.target.files[0] ?? null;
  decodedGif = null;
  if (!selectedFile) {
    clearPreview();
    setState(transport.connected ? 'ready' : 'disconnected');
    return;
  }

  elements['media-badge'].textContent = selectedFile.type === 'image/gif' ? 'GIF' : 'Image';
  try {
    setState('processing');
    setProgress(0, 'Preparing preview');
    if (selectedFile.type === 'image/gif') decodedGif = await decodeGif(selectedFile);
    await refreshPreview();
    log(`Loaded ${selectedFile.name}${decodedGif ? ` · ${decodedGif.frames.length} frames` : ''}`);
    setProgress(0, 'Ready');
    setState(transport.connected ? 'ready' : 'disconnected');
  } catch (error) {
    selectedFile = null;
    event.target.value = '';
    clearPreview();
    log(error.message, 'error');
    setState(transport.connected ? 'ready' : 'disconnected');
  }
}

async function refreshPreview() {
  if (!selectedFile) return;
  const options = imageOptions();
  let prepared;
  if (decodedGif) {
    const iterator = iterateGifFrames(decodedGif);
    const first = await iterator.next();
    prepared = await encodeSourceForDisplay(first.value.canvas, options);
    await iterator.return?.();
  } else {
    prepared = await prepareImageFile(selectedFile, options);
  }
  showPreview(prepared.blob);
  if (prepared.quality < options.quality) {
    log(`JPEG quality reduced to ${Math.round(prepared.quality * 100)}% to fit device memory`);
  }
}

async function displayImage() {
  if (!selectedFile || selectedFile.type === 'image/gif') return;
  const controller = new AbortController();
  playbackController = controller;
  try {
    const persist = shouldPersist();
    setState('processing');
    setProgress(0, 'Encoding image');
    const prepared = await prepareImageFile(selectedFile, imageOptions());
    showPreview(prepared.blob);
    setState('transferring');
    if (persist) await beginPlaylist(1, 1, false);
    await sendFrame(prepared, controller.signal, persist
      ? { persistentFrame: { index: 0, delay: 100 } }
      : {});
    if (persist) await finishPlaylist();
    log(
      `${persist ? 'Saved and displayed' : 'Displayed'} ${prepared.width}×${prepared.height} JPEG (${formatBytes(prepared.bytes.length)})`,
    );
    setProgress(100, persist ? 'Saved on device' : 'Displayed');
  } catch (error) {
    await cancelPersistentUpload();
    if (error.name !== 'AbortError') log(`Display failed: ${error.message}`, 'error');
  } finally {
    playbackController = null;
    setState(transport.connected ? 'ready' : 'disconnected');
  }
}

async function playGif() {
  if (!decodedGif) return;
  const controller = new AbortController();
  playbackController = controller;
  let timingWarningShown = false;
  const persist = shouldPersist();

  try {
    setState('playing');
    if (persist) {
      await beginPlaylist(2, decodedGif.frames.length, elements['loop-input'].checked);
    }
    do {
      for await (const frame of iterateGifFrames(decodedGif)) {
        throwIfAborted(controller.signal);
        const startedAt = performance.now();
        setProgress(
          Math.round((frame.index / decodedGif.frames.length) * 100),
          `Encoding frame ${frame.index + 1} of ${decodedGif.frames.length}`,
        );
        const prepared = await encodeSourceForDisplay(frame.canvas, imageOptions());
        showPreview(prepared.blob);
        const requestedDelay = Number(elements['gif-delay-input'].value) || frame.delay;
        await sendFrame(prepared, controller.signal, {
          label: `Sending frame ${frame.index + 1} of ${decodedGif.frames.length}`,
          frameIndex: frame.index,
          frameCount: decodedGif.frames.length,
          persistentFrame: persist ? { index: frame.index, delay: requestedDelay } : undefined,
        });

        if (persist) continue;
        const elapsed = performance.now() - startedAt;
        if (elapsed > requestedDelay && !timingWarningShown) {
          log(`GIF timing is limited by encode and USB transfer speed (${Math.round(elapsed)} ms per frame)`);
          timingWarningShown = true;
        }
        await abortableDelay(Math.max(0, requestedDelay - elapsed), controller.signal);
      }
      if (persist) {
        await finishPlaylist();
        log(`Saved ${decodedGif.frames.length} GIF frames; playback now runs on the ESP32`);
        break;
      }
    } while (elements['loop-input'].checked && !controller.signal.aborted);
    setProgress(100, persist ? 'Saved and playing on device' : 'Playback complete');
  } catch (error) {
    await cancelPersistentUpload();
    if (error.name !== 'AbortError') log(`GIF playback failed: ${error.message}`, 'error');
  } finally {
    playbackController = null;
    setState(transport.connected ? 'ready' : 'disconnected');
  }
}

function stopPlayback() {
  playbackController?.abort();
  if (activeTransferId && transport.connected) {
    void transport.request(
      Command.CANCEL_TRANSFER,
      createTransferIdPayload(activeTransferId),
    ).catch(() => {});
  }
}

async function beginPlaylist(mediaType, frameCount, loop) {
  await transport.request(Command.BEGIN_PLAYLIST, createBeginPlaylistPayload({
    mediaType,
    frameCount,
    loop,
    rotation: Number(elements['rotation-select'].value),
  }), { timeoutMs: 30000 });
}

async function finishPlaylist() {
  await transport.request(Command.END_PLAYLIST, new Uint8Array(), { timeoutMs: 30000 });
}

async function cancelPersistentUpload() {
  if (!transport.connected) return;
  await transport.request(Command.CANCEL_TRANSFER, new Uint8Array()).catch(() => {});
}

async function sendFrame(prepared, signal, progressContext = {}) {
  throwIfAborted(signal);
  if (prepared.bytes.length > capabilities.maxFrameSize) {
    throw new Error(`Encoded frame exceeds the ${formatBytes(capabilities.maxFrameSize)} device limit`);
  }

  transferId = (transferId + 1) & 0xffff;
  if (transferId === 0) transferId = 1;
  activeTransferId = transferId;
  const rotation = Number(elements['rotation-select'].value);
  const maxDataLength = Math.max(1, Math.min(4090, capabilities.maxChunk));

  try {
    await transport.request(Command.BEGIN_FRAME, createBeginFramePayload({
      transferId,
      width: prepared.width,
      height: prepared.height,
      codec: Codec.JPEG,
      rotation,
      totalSize: prepared.bytes.length,
      frameIndex: progressContext.persistentFrame?.index,
      delayMs: progressContext.persistentFrame?.delay,
    }));

    for (let offset = 0; offset < prepared.bytes.length; offset += maxDataLength) {
      throwIfAborted(signal);
      const chunk = prepared.bytes.subarray(offset, offset + maxDataLength);
      await transport.request(
        Command.FRAME_CHUNK,
        createFrameChunkPayload(transferId, offset, chunk),
      );
      const frameProgress = (offset + chunk.length) / prepared.bytes.length;
      const overallProgress = progressContext.frameCount
        ? ((progressContext.frameIndex + frameProgress) / progressContext.frameCount) * 100
        : frameProgress * 100;
      setProgress(overallProgress, progressContext.label ?? 'Transferring image');
    }

    throwIfAborted(signal);
    await transport.request(
      Command.COMMIT_FRAME,
      createTransferIdPayload(transferId),
      { timeoutMs: 12000 },
    );
  } catch (error) {
    if (transport.connected) {
      await transport.request(
        Command.CANCEL_TRANSFER,
        createTransferIdPayload(transferId),
      ).catch(() => {});
    }
    throw error;
  } finally {
    activeTransferId = null;
  }
}

async function updateBacklight() {
  savePreferences();
  if (!transport.connected || state !== 'ready') return;
  const value = Math.round(Number(elements['backlight-input'].value) * 2.55);
  try {
    await transport.request(Command.SET_BACKLIGHT, Uint8Array.of(value));
    log(`Backlight set to ${elements['backlight-input'].value}%`);
  } catch (error) {
    log(`Backlight update failed: ${error.message}`, 'error');
  }
}

async function clearDisplay() {
  try {
    await transport.request(Command.CLEAR_DISPLAY, createClearPayload(0));
    log('Display cleared');
  } catch (error) {
    log(`Clear failed: ${error.message}`, 'error');
  }
}

async function removeStoredMedia() {
  try {
    await transport.request(Command.CLEAR_STORED, new Uint8Array(), { timeoutMs: 12000 });
    await transport.request(Command.CLEAR_DISPLAY, createClearPayload(0));
    log('Saved media removed');
  } catch (error) {
    log(`Could not remove saved media: ${error.message}`, 'error');
  }
}

function shouldPersist() {
  return capabilities.persistentStorage && elements['persist-input'].checked;
}

function imageOptions() {
  return {
    width: capabilities.width,
    height: capabilities.height,
    rotation: Number(elements['rotation-select'].value),
    fit: elements['fit-select'].value,
    background: elements['background-input'].value,
    quality: Number(elements['quality-input'].value) / 100,
    maxFrameSize: capabilities.maxFrameSize,
  };
}

function setState(nextState) {
  state = nextState;
  const connected = transport.connected;
  const ready = nextState === 'ready';
  const busy = ['processing', 'transferring', 'playing', 'cancelling'].includes(nextState);
  const isGif = selectedFile?.type === 'image/gif';

  elements['connect-button'].disabled = nextState !== 'disconnected' || !webSerialAvailable;
  elements['disconnect-button'].disabled = !connected || busy;
  elements['display-button'].disabled = !ready || !selectedFile || isGif;
  elements['play-button'].disabled = !ready || !decodedGif;
  elements['stop-button'].disabled = !busy;
  elements['clear-button'].disabled = !ready;
  elements['remove-stored-button'].disabled = !ready || !capabilities.persistentStorage;
  elements['media-input'].disabled = busy;

  const online = connected && !['disconnecting', 'disconnected'].includes(nextState);
  elements['connection-pill'].className = `pill ${online ? 'pill-online' : 'pill-offline'}`;
  elements['connection-pill'].textContent = stateLabel(nextState);
}

function updateDeviceDetails() {
  elements['device-resolution'].textContent = `${capabilities.width} × ${capabilities.height}`;
  elements['device-firmware'].textContent = capabilities.firmwareVersion;
  elements['device-codec'].textContent = capabilities.codecs.jpeg ? 'JPEG' : 'Unsupported';
  elements['device-storage'].textContent = capabilities.persistentStorage
    ? `${formatBytes(capabilities.maxStoredBytes)} flash`
    : 'Unavailable';
  elements['persist-input'].disabled = !capabilities.persistentStorage;

  for (const option of elements['rotation-select'].options) {
    option.disabled = (capabilities.rotationMask & (1 << Number(option.value))) === 0;
  }
}

function updateSupportMessage() {
  let message = '';
  if (!globalThis.isSecureContext) {
    message = 'Web Serial requires HTTPS or localhost. Open this app from a secure origin.';
  } else if (!('serial' in navigator)) {
    message = 'Web Serial is unavailable. Use the current desktop version of Chrome or Edge.';
  }
  elements['support-message'].hidden = !message;
  elements['support-message'].textContent = message;
}

function updateRangeOutputs() {
  elements['quality-output'].value = elements['quality-input'].value;
  elements['backlight-output'].value = elements['backlight-input'].value;
}

function showPreview(blob) {
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = URL.createObjectURL(blob);
  elements['preview-image'].src = previewUrl;
  elements['preview-image'].hidden = false;
  elements['preview-empty'].hidden = true;
}

function clearPreview() {
  if (previewUrl) URL.revokeObjectURL(previewUrl);
  previewUrl = null;
  elements['preview-image'].removeAttribute('src');
  elements['preview-image'].hidden = true;
  elements['preview-empty'].hidden = false;
  elements['media-badge'].textContent = 'No media';
}

function setProgress(value, label) {
  const normalized = Math.max(0, Math.min(100, Math.round(value)));
  elements['transfer-progress'].value = normalized;
  elements['progress-value'].textContent = `${normalized}%`;
  elements['progress-label'].textContent = label;
}

function log(message, level = 'info') {
  const entry = elements['log-template'].content.firstElementChild.cloneNode(true);
  entry.dataset.level = level;
  entry.querySelector('time').textContent = new Date().toLocaleTimeString();
  entry.querySelector('span').textContent = message;
  elements['activity-log'].prepend(entry);
}

function savePreferences() {
  localStorage.setItem('cyberclip-preferences', JSON.stringify({
    fit: elements['fit-select'].value,
    rotation: elements['rotation-select'].value,
    background: elements['background-input'].value,
    quality: elements['quality-input'].value,
    backlight: elements['backlight-input'].value,
    gifDelay: elements['gif-delay-input'].value,
    loop: elements['loop-input'].checked,
    persist: elements['persist-input'].checked,
  }));
}

function restorePreferences() {
  try {
    const values = JSON.parse(localStorage.getItem('cyberclip-preferences'));
    if (!values) return;
    elements['fit-select'].value = values.fit ?? 'contain';
    elements['rotation-select'].value = values.rotation ?? '0';
    elements['background-input'].value = values.background ?? '#000000';
    elements['quality-input'].value = values.quality ?? '82';
    elements['backlight-input'].value = values.backlight ?? '100';
    elements['gif-delay-input'].value = values.gifDelay ?? '';
    elements['loop-input'].checked = values.loop ?? true;
    elements['persist-input'].checked = values.persist ?? true;
  } catch {
    localStorage.removeItem('cyberclip-preferences');
  }
}

function stateLabel(value) {
  return {
    disconnected: 'Disconnected',
    connecting: 'Choose device',
    handshaking: 'Identifying device',
    ready: 'Ready',
    processing: 'Processing',
    transferring: 'Transferring',
    playing: 'Playing GIF',
    cancelling: 'Stopping',
    disconnecting: 'Disconnecting',
  }[value] ?? value;
}

function throwIfAborted(signal) {
  if (signal?.aborted) throw new DOMException('Operation cancelled', 'AbortError');
}

function abortableDelay(milliseconds, signal) {
  throwIfAborted(signal);
  return new Promise((resolve, reject) => {
    const handleAbort = () => {
      clearTimeout(timeout);
      reject(new DOMException('Operation cancelled', 'AbortError'));
    };
    const timeout = setTimeout(() => {
      signal?.removeEventListener('abort', handleAbort);
      resolve();
    }, milliseconds);
    signal?.addEventListener('abort', handleAbort, { once: true });
  });
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  return `${(bytes / 1024).toFixed(1)} KB`;
}
