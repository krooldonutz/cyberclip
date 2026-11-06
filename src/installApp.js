import './styles.css';
import './site.css';
import {
  DOWNLOAD_PORT_FILTER,
  DownloadPortNotAuthorizedError,
  InstallInterruptedError,
  ESPRESSIF_USB_VENDOR_ID,
  USB_JTAG_SERIAL_PRODUCT_ID,
  flashBundledFirmware,
  isDownloadPort,
  loadFirmwareVersions,
} from './flasher.js';
import { SerialTransport } from './serial.js';
import { registerServiceWorker } from './serviceWorker.js';

const elements = Object.fromEntries([
  'support-message',
  'install-notice',
  'install-notice-message',
  'install-notice-actions',
  'continue-install-button',
  'cancel-install-button',
  'versions-status',
  'version-list',
  'version-template',
  'flash-button',
  'install-progress',
  'progress-label',
  'progress-value',
  'clear-log-button',
  'activity-log',
  'log-template',
].map((id) => [id, document.getElementById(id)]));

const WAITING_MESSAGE =
  'Your board is waiting in update mode. Select Install firmware to finish installing; your WiFi settings are kept.';

const webSerialAvailable = globalThis.isSecureContext && 'serial' in navigator;
let versions = [];
let installing = false;
let awaitingDownloadPort = false;
let askingForDownloadPort = false;

initialize();

function initialize() {
  updateSupportMessage();
  setProgress(0, 'Ready');
  elements['flash-button'].addEventListener('click', () => installFirmware());
  elements['clear-log-button'].addEventListener('click', () => {
    elements['activity-log'].replaceChildren();
  });
  elements['version-list'].addEventListener('change', updateButtons);
  if (webSerialAvailable) {
    // A board left in download mode (e.g. by an install that was stopped)
    // shows up as the USB-Serial/JTAG unit; point the user at finishing it.
    navigator.serial.addEventListener('connect', () => { void checkForWaitingBoard(); });
    navigator.serial.addEventListener('disconnect', () => { void checkForWaitingBoard(); });
  }
  void checkForWaitingBoard();
  void showVersions();
  registerServiceWorker(log);
}

async function showVersions() {
  try {
    versions = await loadFirmwareVersions();
  } catch (error) {
    elements['versions-status'].textContent = `Firmware versions could not be loaded: ${error.message}`;
    log(`Firmware versions could not be loaded: ${error.message}`, 'error');
    return;
  }
  const items = versions.map((release) => {
    const item = elements['version-template'].content.firstElementChild.cloneNode(true);
    const input = item.querySelector('input');
    input.value = release.version;
    input.checked = release.latest;
    item.querySelector('.version-name').textContent = release.version;
    item.querySelector('.version-latest').hidden = !release.latest;
    const size = release.builds.reduce((sum, build) => sum + build.size, 0);
    item.querySelector('.version-size').textContent = formatMegabytes(size);
    return item;
  });
  elements['version-list'].replaceChildren(...items);
  elements['versions-status'].textContent = versions.length === 1
    ? '1 version available.'
    : `${versions.length} versions available, newest first.`;
  updateButtons();
}

function selectedVersion() {
  return elements['version-list'].querySelector('input:checked')?.value ?? null;
}

function updateButtons() {
  elements['flash-button'].disabled = installing || !webSerialAvailable || !selectedVersion();
  for (const input of elements['version-list'].querySelectorAll('input')) {
    input.disabled = installing;
  }
  const version = selectedVersion();
  const release = versions.find((candidate) => candidate.version === version);
  elements['flash-button'].textContent = release
    ? `Install firmware ${release.version}`
    : 'Install firmware';
}

function showInstallNotice(message, { actions = false } = {}) {
  elements['install-notice-message'].textContent = message;
  elements['install-notice-actions'].hidden = !actions;
  elements['install-notice'].hidden = false;
}

function hideInstallNotice() {
  elements['install-notice'].hidden = true;
  elements['install-notice-actions'].hidden = true;
}

// Shows a notice while a board sits in download mode outside an install, so
// a stopped install can always be finished from here (no BOOT/RST needed).
async function checkForWaitingBoard() {
  if (!webSerialAvailable || installing || askingForDownloadPort) return;
  const ports = await navigator.serial.getPorts().catch(() => []);
  if (ports.some(isDownloadPort)) {
    awaitingDownloadPort = true;
    showInstallNotice(WAITING_MESSAGE);
  } else if (!awaitingDownloadPort) {
    hideInstallNotice();
  }
}

// The browser only opens the port picker right after a click, and the click
// on Install firmware has been used up by the time the board restarts into
// download mode. This asks for a fresh click instead of failing.
function askForDownloadPort({ signal }) {
  return new Promise((resolve, reject) => {
    askingForDownloadPort = true;
    const finish = () => {
      askingForDownloadPort = false;
      elements['continue-install-button'].onclick = null;
      elements['cancel-install-button'].onclick = null;
      hideInstallNotice();
    };
    showInstallNotice(
      'The board restarted into update mode. Select Continue installation, then choose "USB JTAG/serial debug unit".',
      { actions: true },
    );
    elements['continue-install-button'].onclick = async () => {
      try {
        const picked = await navigator.serial.requestPort({ filters: [DOWNLOAD_PORT_FILTER] });
        if (isDownloadPort(picked)) {
          finish();
          resolve(picked);
        }
      } catch {
        // Picker closed without a choice; keep waiting for another click.
      }
    };
    elements['cancel-install-button'].onclick = () => {
      finish();
      reject(new DOMException('Installation cancelled', 'AbortError'));
    };
    signal.addEventListener('abort', () => {
      if (askingForDownloadPort) finish();
      resolve(null);
    }, { once: true });
  });
}

async function chooseFlashPort() {
  const authorized = await navigator.serial.getPorts();
  const downloadPort = authorized.find(isDownloadPort);
  if (downloadPort) return downloadPort;
  return navigator.serial.requestPort(
    awaitingDownloadPort ? { filters: [DOWNLOAD_PORT_FILTER] } : undefined,
  );
}

function setInstalling(value) {
  installing = value;
  updateButtons();
}

async function installFirmware() {
  const version = selectedVersion();
  if (!version) return;
  const older = versions.find((candidate) => candidate.latest)?.version !== version;
  if (!globalThis.confirm(
    `Install Cyberclip firmware ${version}${older ? ' (an older version)' : ''}? Your WiFi settings and saved media are kept. Keep the USB cable connected until installation finishes.`,
  )) {
    return;
  }

  try {
    const port = await chooseFlashPort();
    hideInstallNotice();
    setInstalling(true);
    setProgress(0, 'Preparing firmware');
    const result = await flashBundledFirmware({
      port,
      version,
      requestDownloadPort: askForDownloadPort,
      onProgress(progress) {
        setProgress(progress, 'Installing firmware');
      },
      onStatus(message) {
        setProgress(elements['install-progress'].value, message);
      },
    });
    const target = result.board ?? result.chip;
    awaitingDownloadPort = false;
    log(`Installed Cyberclip firmware ${result.version}${target ? ` for ${target}` : ''}`);
    setProgress(100, 'Firmware installed');
    await confirmInstalledVersion();
  } catch (error) {
    const leftInDownloadMode = error instanceof DownloadPortNotAuthorizedError
      || error instanceof InstallInterruptedError;
    awaitingDownloadPort = leftInDownloadMode;
    const message = error.name === 'NotFoundError'
      ? 'No serial device was selected'
      : error.message;
    log(`Firmware installation failed: ${message}`, 'error');
    setProgress(0, 'Firmware installation failed');
    if (leftInDownloadMode) showInstallNotice(WAITING_MESSAGE);
  } finally {
    setInstalling(false);
    void checkForWaitingBoard();
  }
}

// The native USB port re-enumerates as a new "Pixie Pixel Gear" device once
// the new firmware starts. Asks it for its version, then lets it go so the
// upload page can connect.
async function confirmInstalledVersion() {
  const transport = new SerialTransport();
  for (let attempt = 0; attempt < 10; attempt += 1) {
    await delay(1000);
    const ports = (await navigator.serial.getPorts()).filter((candidate) => {
      const info = candidate.getInfo?.() ?? {};
      return info.usbVendorId === ESPRESSIF_USB_VENDOR_ID
        && info.usbProductId !== USB_JTAG_SERIAL_PRODUCT_ID;
    });
    for (const candidate of ports) {
      try {
        const details = await transport.connect({ port: candidate });
        log(`The board is running firmware ${details.firmwareVersion}. Open Upload media to send images and GIFs.`);
        await transport.disconnect();
        return;
      } catch {
        // Try the next port or wait for the board to finish booting.
      }
    }
  }
  log('Firmware installed. Open Upload media and choose "Pixie Pixel Gear" to connect.');
}

function updateSupportMessage() {
  let message = '';
  if (!globalThis.isSecureContext) {
    message = 'Web Serial requires HTTPS or localhost. Open this page from a secure origin.';
  } else if (!('serial' in navigator)) {
    message = 'Installing firmware needs Web Serial. Use the current desktop version of Chrome or Edge.';
  }
  elements['support-message'].hidden = !message;
  elements['support-message'].textContent = message;
}

function setProgress(value, label) {
  const normalized = Math.max(0, Math.min(100, Math.round(value)));
  elements['install-progress'].value = normalized;
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

function formatMegabytes(bytes) {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function delay(milliseconds) {
  return new Promise((resolve) => { setTimeout(resolve, milliseconds); });
}
