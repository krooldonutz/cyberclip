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
import { WifiTransport } from './wifiTransport.js';

const ids = [
  'connection-pill', 'media-badge', 'preview-image', 'preview-empty', 'media-input',
  'display-button', 'play-button', 'stop-button', 'clear-button', 'remove-stored-button',
  'transfer-progress', 'progress-label', 'progress-value', 'fit-select', 'rotation-select',
  'background-input', 'quality-input', 'quality-output', 'backlight-input',
  'backlight-output', 'gif-delay-input', 'loop-input', 'persist-input', 'retry-button',
  'activity-log', 'log-template',
];
const elements = Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));
const DEFAULT_CAPABILITIES = {
  width: 170,
  height: 320,
  maxChunk: 1024,
  maxFrameSize: 128 * 1024,
  rotationMask: 0x0f,
  persistentStorage: true,
};

let state = 'disconnected';
let capabilities = DEFAULT_CAPABILITIES;
let selectedFile = null;
let decodedGif = null;
let previewUrl = null;
let transferId = 0;
let activeTransferId = null;
let operationController = null;

const transport = new WifiTransport({
  onStateChange(nextState, details) {
    if (nextState === 'ready') {
      capabilities = details;
      elements['persist-input'].disabled = !details.persistentStorage;
      for (const option of elements['rotation-select'].options) {
        option.disabled = (details.rotationMask & (1 << Number(option.value))) === 0;
      }
      log(`Connected to ${details.deviceName} · firmware ${details.firmwareVersion}`);
      setProgress(0, 'Ready');
    }
    if (nextState === 'disconnected' && details) log(details.message, 'error');
    if (['connecting', 'handshaking', 'ready', 'disconnected'].includes(nextState)) {
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
  bindEvents();
  setState('connecting');
  void connect();
}

function bindEvents() {
  elements['retry-button'].addEventListener('click', connect);
  elements['media-input'].addEventListener('change', handleMediaSelection);
  elements['display-button'].addEventListener('click', displayImage);
  elements['play-button'].addEventListener('click', playGif);
  elements['stop-button'].addEventListener('click', stopOperation);
  elements['clear-button'].addEventListener('click', clearDisplay);
  elements['remove-stored-button'].addEventListener('click', removeStoredMedia);

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
}

async function connect() {
  try {
    if (transport.connected) await transport.disconnect();
    await transport.connect({
      host: globalThis.location.host || '192.168.4.1',
      trustedDevicePage: true,
    });
  } catch (error) {
    log(`Connection failed: ${error.message}`, 'error');
    setProgress(0, 'Reconnect to the Cyberclip Wi-Fi and try again');
    setState('disconnected');
  }
}

async function handleMediaSelection(event) {
  stopOperation();
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
  } catch (error) {
    selectedFile = null;
    event.target.value = '';
    clearPreview();
    log(error.message, 'error');
  } finally {
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
  if (!selectedFile || decodedGif) return;
  const controller = new AbortController();
  operationController = controller;
  const persist = shouldPersist();
  try {
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
    setProgress(100, persist ? 'Saved on device' : 'Displayed');
    log(`${persist ? 'Saved and displayed' : 'Displayed'} ${prepared.width}×${prepared.height} JPEG`);
  } catch (error) {
    await cancelUpload();
    if (error.name !== 'AbortError') log(`Display failed: ${error.message}`, 'error');
  } finally {
    operationController = null;
    setState(transport.connected ? 'ready' : 'disconnected');
  }
}

async function playGif() {
  if (!decodedGif) return;
  const controller = new AbortController();
  operationController = controller;
  const persist = shouldPersist();
  let nextFrameAt = performance.now();
  try {
    setState('playing');
    if (persist) await beginPlaylist(2, decodedGif.frames.length, elements['loop-input'].checked);
    do {
      for await (const frame of iterateGifFrames(decodedGif)) {
        throwIfAborted(controller.signal);
        const delay = Number(elements['gif-delay-input'].value) || frame.delay;
        const prepared = await encodeSourceForDisplay(frame.canvas, imageOptions());
        showPreview(prepared.blob);
        await sendFrame(prepared, controller.signal, {
          label: `Sending frame ${frame.index + 1} of ${decodedGif.frames.length}`,
          frameIndex: frame.index,
          frameCount: decodedGif.frames.length,
          persistentFrame: persist ? { index: frame.index, delay } : undefined,
        });
        if (!persist) {
          nextFrameAt += delay;
          await abortableDelay(Math.max(0, nextFrameAt - performance.now()), controller.signal);
        }
      }
      if (persist) {
        await finishPlaylist();
        break;
      }
    } while (elements['loop-input'].checked && !controller.signal.aborted);
    setProgress(100, persist ? 'Saved and playing on device' : 'Playback complete');
  } catch (error) {
    await cancelUpload();
    if (error.name !== 'AbortError') log(`GIF playback failed: ${error.message}`, 'error');
  } finally {
    operationController = null;
    setState(transport.connected ? 'ready' : 'disconnected');
  }
}

function stopOperation() {
  operationController?.abort();
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

async function cancelUpload() {
  if (!transport.connected) return;
  await transport.request(Command.CANCEL_TRANSFER, new Uint8Array()).catch(() => {});
}

async function sendFrame(prepared, signal, context = {}) {
  throwIfAborted(signal);
  transferId = (transferId + 1) & 0xffff || 1;
  activeTransferId = transferId;
  const maxChunk = Math.max(1, Math.min(4090, capabilities.maxChunk));
  try {
    await transport.request(Command.BEGIN_FRAME, createBeginFramePayload({
      transferId,
      width: prepared.width,
      height: prepared.height,
      codec: Codec.JPEG,
      rotation: Number(elements['rotation-select'].value),
      totalSize: prepared.bytes.length,
      frameIndex: context.persistentFrame?.index,
      delayMs: context.persistentFrame?.delay,
    }));
    for (let offset = 0; offset < prepared.bytes.length; offset += maxChunk) {
      throwIfAborted(signal);
      const chunk = prepared.bytes.subarray(offset, offset + maxChunk);
      await transport.request(
        Command.FRAME_CHUNK,
        createFrameChunkPayload(transferId, offset, chunk),
      );
      const frameProgress = (offset + chunk.length) / prepared.bytes.length;
      const progress = context.frameCount
        ? ((context.frameIndex + frameProgress) / context.frameCount) * 100
        : frameProgress * 100;
      setProgress(progress, context.label ?? 'Transferring image');
    }
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
  try {
    const value = Math.round(Number(elements['backlight-input'].value) * 2.55);
    await transport.request(Command.SET_BACKLIGHT, Uint8Array.of(value));
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

function shouldPersist() {
  return capabilities.persistentStorage && elements['persist-input'].checked;
}

function setState(nextState) {
  state = nextState;
  const ready = nextState === 'ready';
  const busy = ['processing', 'transferring', 'playing'].includes(nextState);
  const isGif = selectedFile?.type === 'image/gif';
  elements['media-input'].disabled = busy;
  elements['display-button'].disabled = !ready || !selectedFile || isGif;
  elements['play-button'].disabled = !ready || !decodedGif;
  elements['stop-button'].disabled = !busy;
  elements['clear-button'].disabled = !ready;
  elements['remove-stored-button'].disabled = !ready || !capabilities.persistentStorage;
  elements['retry-button'].disabled = busy || nextState === 'connecting' || nextState === 'handshaking';
  const online = transport.connected && nextState !== 'disconnected';
  elements['connection-pill'].className = `pill ${online ? 'pill-online' : 'pill-offline'}`;
  elements['connection-pill'].textContent = {
    connecting: 'Connecting',
    handshaking: 'Identifying',
    ready: 'Ready',
    processing: 'Processing',
    transferring: 'Transferring',
    playing: 'Playing GIF',
    disconnected: 'Disconnected',
  }[nextState] ?? nextState;
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

function updateRangeOutputs() {
  elements['quality-output'].value = elements['quality-input'].value;
  elements['backlight-output'].value = elements['backlight-input'].value;
}

function savePreferences() {
  localStorage.setItem('cyberclip-device-preferences', JSON.stringify({
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
    const values = JSON.parse(localStorage.getItem('cyberclip-device-preferences'));
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
    localStorage.removeItem('cyberclip-device-preferences');
  }
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
