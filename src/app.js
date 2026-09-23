import './styles.css';
import { flashBundledFirmware } from './flasher.js';
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
  createWifiCredentialsPayload,
  parseWifiStatusResponse,
  WifiState,
} from './protocol.js';
import { SerialTransport } from './serial.js';
import { WifiTransport } from './wifiTransport.js';

const DEFAULT_CAPABILITIES = {
  width: 170,
  height: 320,
  maxChunk: 4096,
  maxFrameSize: 128 * 1024,
  rotationMask: 0x0f,
  codecs: { jpeg: true },
};

const WIFI_PAIRINGS_KEY = 'cyberclip-wifi-pairings';
const WIFI_LAST_HOST_KEY = 'cyberclip-wifi-last-host';

const elements = Object.fromEntries([
  'connection-pill',
  'install-button',
  'connect-button',
  'connect-wifi-button',
  'setup-wifi-button',
  'forget-wifi-button',
  'wifi-host-input',
  'disconnect-button',
  'flash-button',
  'support-message',
  'device-resolution',
  'device-firmware',
  'device-transport',
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

function handleTransportStateChange(nextState, details) {
  if (nextState === 'ready') {
    capabilities = details;
    updateDeviceDetails();
    const via = activeTransport === wifiTransport ? 'WiFi' : 'USB';
    log(`Connected to ${details.deviceName} with firmware ${details.firmwareVersion} over ${via}`);
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
}

function handleTransportProtocolError(error) {
  log(error.message, 'error');
}

const serialTransport = new SerialTransport({
  onStateChange: handleTransportStateChange,
  onProtocolError: handleTransportProtocolError,
});
const wifiTransport = new WifiTransport({
  onStateChange: handleTransportStateChange,
  onProtocolError: handleTransportProtocolError,
});
let activeTransport = serialTransport;

initialize();

function initialize() {
  restorePreferences();
  updateRangeOutputs();
  setState('disconnected');
  updateSupportMessage();
  setProgress(0, 'Ready');
  bindEvents();

  const lastHost = localStorage.getItem(WIFI_LAST_HOST_KEY);
  if (lastHost) elements['wifi-host-input'].value = lastHost;

  if (!applyWifiHandoffFromUrl() && lastHost && loadWifiPairing(lastHost)?.token) {
    log(`Reconnecting to ${lastHost} over WiFi...`);
    void connectWifi();
  }

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
  elements['connect-button'].addEventListener('click', connectUsb);
  elements['connect-wifi-button'].addEventListener('click', connectWifi);
  elements['setup-wifi-button'].addEventListener('click', setupWifi);
  elements['forget-wifi-button'].addEventListener('click', forgetWifi);
  elements['disconnect-button'].addEventListener('click', () => activeTransport.disconnect());
  elements['flash-button'].addEventListener('click', installFirmware);
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

async function installFirmware() {
  if (!globalThis.confirm(
    'Install the bundled Cyberclip firmware? Keep the USB cable connected until installation finishes.',
  )) {
    return;
  }

  let port = serialTransport.port;
  try {
    if (!port) port = await navigator.serial.requestPort();
    if (activeTransport.connected) await activeTransport.disconnect();
    setState('flashing');
    setProgress(0, 'Preparing firmware');
    const manifest = await flashBundledFirmware({
      port,
      onProgress(progress) {
        setProgress(progress, 'Installing firmware');
      },
      onStatus(message) {
        setProgress(elements['transfer-progress'].value, message);
      },
    });
    log(`Installed Cyberclip firmware ${manifest.version}`);
    setProgress(100, 'Firmware installed');
    await abortableDelay(1000);
    activeTransport = serialTransport;
    await serialTransport.connect({ port });
  } catch (error) {
    const message = error.name === 'NotFoundError'
      ? 'No serial device was selected'
      : error.message;
    log(`Firmware installation failed: ${message}`, 'error');
    setProgress(0, 'Firmware installation failed');
    setState('disconnected');
  }
}

async function connectUsb() {
  activeTransport = serialTransport;
  let lastError;
  try {
    const authorizedPorts = await navigator.serial.getPorts();
    for (const port of authorizedPorts) {
      try {
        await serialTransport.connect({ port });
        return;
      } catch (error) {
        lastError = error;
      }
    }
    if (authorizedPorts.length === 0) {
      await serialTransport.connect();
      return;
    }
    throw lastError;
  } catch (error) {
    const reportedError = error.name === 'NotFoundError' ? lastError : error;
    if (reportedError) log(`Connection failed: ${reportedError.message}`, 'error');
    setState('disconnected');
  }
}

// Handles the handoff from the board's own QR code (phone-only setup, no
// USB/computer involved - see firmware/src/setup_portal.h): the QR link
// carries the device's IP and pairing token as query params so scanning it
// can connect immediately, with no typing.
function applyWifiHandoffFromUrl() {
  const params = new URLSearchParams(window.location.search);
  const host = params.get('host');
  const token = params.get('token')?.toLowerCase();
  if (!host || !/^[0-9a-f]{32}$/.test(token ?? '')) return false;

  history.replaceState(null, '', window.location.pathname);
  saveWifiPairing(host, token);
  log(`Scanned WiFi setup for ${host}, connecting...`);
  void connectWifi();
  return true;
}

async function connectWifi() {
  const host = elements['wifi-host-input'].value.trim();
  if (!host) {
    log('Enter the device IP address or hostname first', 'error');
    return;
  }
  const pairing = loadWifiPairing(host);
  if (!pairing?.token) {
    log('Pair over USB first (Set up WiFi) to get a token for this device', 'error');
    return;
  }
  if (serialTransport.connected) await serialTransport.disconnect();
  activeTransport = wifiTransport;
  try {
    await wifiTransport.connect({ host, token: hexToBytes(pairing.token) });
    localStorage.setItem(WIFI_LAST_HOST_KEY, host);
  } catch (error) {
    log(`WiFi connection failed: ${error.message}`, 'error');
    setState('disconnected');
  }
}

async function setupWifi() {
  if (!serialTransport.connected) {
    log('Connect over USB first to set up WiFi', 'error');
    return;
  }
  const ssid = globalThis.prompt('WiFi network name (SSID)');
  if (!ssid) return;
  const password = globalThis.prompt('WiFi password (leave blank for an open network)') ?? '';

  try {
    setProgress(0, 'Sending WiFi credentials');
    const response = await serialTransport.request(
      Command.SET_WIFI_CREDENTIALS,
      createWifiCredentialsPayload({ ssid, password }),
      { expectedCommand: Command.WIFI_STATUS_RESPONSE, timeoutMs: 20000 },
    );
    let wifiStatus = parseWifiStatusResponse(response.payload);
    const token = wifiStatus.token ? bytesToHex(wifiStatus.token) : findAnyStoredWifiToken();
    if (!token) {
      throw new Error('This device already has a WiFi pairing token from another browser. Use "Forget WiFi" and set up again to issue a new one.');
    }

    setProgress(20, `Connecting to "${ssid}"`);
    for (let attempt = 0; attempt < 15 && wifiStatus.state === WifiState.CONNECTING; attempt += 1) {
      await abortableDelay(1000);
      const statusResponse = await serialTransport.request(
        Command.GET_WIFI_STATUS,
        new Uint8Array(),
        { expectedCommand: Command.WIFI_STATUS_RESPONSE },
      );
      wifiStatus = parseWifiStatusResponse(statusResponse.payload);
      setProgress(20 + attempt * 5, `Connecting to "${ssid}"`);
    }
    if (wifiStatus.state !== WifiState.CONNECTED) {
      throw new Error(`Could not join "${ssid}" - check the network name and password`);
    }

    const host = `${wifiStatus.hostname}.local`;
    saveWifiPairing(host, token);
    log(`WiFi ready at ${host} (try the IP ${wifiStatus.ip} instead if that doesn't resolve). Switching to it now...`);
    setProgress(100, 'WiFi ready');
    await connectWifi();
  } catch (error) {
    log(`WiFi setup failed: ${error.message}`, 'error');
    setProgress(0, 'Ready');
  }
}

async function forgetWifi() {
  if (!activeTransport.connected) {
    log('Connect first to forget WiFi', 'error');
    return;
  }
  const viaWifi = activeTransport === wifiTransport;
  try {
    await activeTransport.request(Command.CLEAR_WIFI_CREDENTIALS, new Uint8Array());
    log('The device forgot its WiFi network. Set up WiFi again (over USB) to reconnect wirelessly.');
    if (viaWifi) log('That was sent over WiFi, so this browser will now disconnect.');
  } catch (error) {
    log(`Could not forget WiFi: ${error.message}`, 'error');
  }
}

async function handleMediaSelection(event) {
  stopPlayback();
  selectedFile = event.target.files[0] ?? null;
  decodedGif = null;
  if (!selectedFile) {
    clearPreview();
    setState(activeTransport.connected ? 'ready' : 'disconnected');
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
    setState(activeTransport.connected ? 'ready' : 'disconnected');
  } catch (error) {
    selectedFile = null;
    event.target.value = '';
    clearPreview();
    log(error.message, 'error');
    setState(activeTransport.connected ? 'ready' : 'disconnected');
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
    setState(activeTransport.connected ? 'ready' : 'disconnected');
  }
}

async function playGif() {
  if (!decodedGif) return;
  const controller = new AbortController();
  playbackController = controller;
  let timingWarningShown = false;
  const persist = shouldPersist();
  let nextFrameAt = performance.now();
  let hasDisplayedFrame = false;

  try {
    setState('playing');
    if (persist) {
      await beginPlaylist(2, decodedGif.frames.length, elements['loop-input'].checked);
    }
    do {
      for await (const frame of iterateGifFrames(decodedGif)) {
        throwIfAborted(controller.signal);
        const requestedDelay = Number(elements['gif-delay-input'].value) || frame.delay;
        const frameEndsAt = nextFrameAt + requestedDelay;
        if (!persist && hasDisplayedFrame && performance.now() >= frameEndsAt) {
          nextFrameAt = frameEndsAt;
          if (!timingWarningShown) {
            log('GIF timing is limited by encode and transfer speed; late frames will be skipped');
            timingWarningShown = true;
          }
          continue;
        }

        const startedAt = performance.now();
        setProgress(
          Math.round((frame.index / decodedGif.frames.length) * 100),
          `Encoding frame ${frame.index + 1} of ${decodedGif.frames.length}`,
        );
        const prepared = await encodeSourceForDisplay(frame.canvas, imageOptions());
        showPreview(prepared.blob);
        await sendFrame(prepared, controller.signal, {
          label: `Sending frame ${frame.index + 1} of ${decodedGif.frames.length}`,
          frameIndex: frame.index,
          frameCount: decodedGif.frames.length,
          persistentFrame: persist ? { index: frame.index, delay: requestedDelay } : undefined,
        });
        hasDisplayedFrame = true;

        if (persist) continue;
        const completedAt = performance.now();
        const elapsed = completedAt - startedAt;
        nextFrameAt = frameEndsAt;
        if (elapsed > requestedDelay && !timingWarningShown) {
          log(`GIF timing is limited by encode and transfer speed (${Math.round(elapsed)} ms per frame); late frames will be skipped`);
          timingWarningShown = true;
        }
        await abortableDelay(Math.max(0, nextFrameAt - completedAt), controller.signal);
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
    setState(activeTransport.connected ? 'ready' : 'disconnected');
  }
}

function stopPlayback() {
  playbackController?.abort();
  if (activeTransferId && activeTransport.connected) {
    void activeTransport.request(
      Command.CANCEL_TRANSFER,
      createTransferIdPayload(activeTransferId),
    ).catch(() => {});
  }
}

async function beginPlaylist(mediaType, frameCount, loop) {
  await activeTransport.request(Command.BEGIN_PLAYLIST, createBeginPlaylistPayload({
    mediaType,
    frameCount,
    loop,
    rotation: Number(elements['rotation-select'].value),
  }), { timeoutMs: 30000 });
}

async function finishPlaylist() {
  await activeTransport.request(Command.END_PLAYLIST, new Uint8Array(), { timeoutMs: 30000 });
}

async function cancelPersistentUpload() {
  if (!activeTransport.connected) return;
  await activeTransport.request(Command.CANCEL_TRANSFER, new Uint8Array()).catch(() => {});
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
    await activeTransport.request(Command.BEGIN_FRAME, createBeginFramePayload({
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
      await activeTransport.request(
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
    await activeTransport.request(
      Command.COMMIT_FRAME,
      createTransferIdPayload(transferId),
      { timeoutMs: 12000 },
    );
  } catch (error) {
    if (activeTransport.connected) {
      await activeTransport.request(
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
  if (!activeTransport.connected || state !== 'ready') return;
  const value = Math.round(Number(elements['backlight-input'].value) * 2.55);
  try {
    await activeTransport.request(Command.SET_BACKLIGHT, Uint8Array.of(value));
    log(`Backlight set to ${elements['backlight-input'].value}%`);
  } catch (error) {
    log(`Backlight update failed: ${error.message}`, 'error');
  }
}

async function clearDisplay() {
  try {
    await activeTransport.request(Command.CLEAR_DISPLAY, createClearPayload(0));
    log('Display cleared');
  } catch (error) {
    log(`Clear failed: ${error.message}`, 'error');
  }
}

async function removeStoredMedia() {
  try {
    await activeTransport.request(Command.CLEAR_STORED, new Uint8Array(), { timeoutMs: 12000 });
    await activeTransport.request(Command.CLEAR_DISPLAY, createClearPayload(0));
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
  const connected = activeTransport.connected;
  const ready = nextState === 'ready';
  const busy = ['processing', 'transferring', 'playing', 'cancelling', 'flashing'].includes(nextState);
  const isGif = selectedFile?.type === 'image/gif';
  const connectedViaUsb = serialTransport.connected;

  elements['connect-button'].disabled = nextState !== 'disconnected' || !webSerialAvailable;
  elements['connect-wifi-button'].disabled = nextState !== 'disconnected';
  elements['setup-wifi-button'].disabled = !ready || !connectedViaUsb;
  // Unlike Set up WiFi, forgetting doesn't need to survive the connection
  // it was sent over, so it works from either transport.
  elements['forget-wifi-button'].disabled = !ready;
  elements['disconnect-button'].disabled = !connected || busy;
  elements['flash-button'].disabled = busy || !webSerialAvailable;
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
  elements['device-transport'].textContent = activeTransport === wifiTransport
    ? `WiFi · ${wifiTransport.host}`
    : 'USB · 921600 baud';
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
    message = 'Web Serial is unavailable. Use the current desktop version of Chrome or Edge. WiFi control still works once a device has been paired.';
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

function loadWifiPairings() {
  try {
    return JSON.parse(localStorage.getItem(WIFI_PAIRINGS_KEY)) ?? {};
  } catch {
    return {};
  }
}

function loadWifiPairing(host) {
  return loadWifiPairings()[host] ?? null;
}

function findAnyStoredWifiToken() {
  const [first] = Object.values(loadWifiPairings());
  return first?.token ?? null;
}

function saveWifiPairing(host, token) {
  const pairings = loadWifiPairings();
  pairings[host] = { token };
  localStorage.setItem(WIFI_PAIRINGS_KEY, JSON.stringify(pairings));
  localStorage.setItem(WIFI_LAST_HOST_KEY, host);
  elements['wifi-host-input'].value = host;
}

function bytesToHex(bytes) {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function hexToBytes(hex) {
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; i += 1) {
    bytes[i] = Number.parseInt(hex.substr(i * 2, 2), 16);
  }
  return bytes;
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
    flashing: 'Installing firmware',
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
