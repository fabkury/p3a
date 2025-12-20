/**
 * p3a Web Flasher
 * 
 * Uses esptool-js to flash ESP32-P4 devices directly from the browser.
 * Requires Chrome, Edge, or Opera with Web Serial API support.
 * 
 * Note: Firmware files are loaded as Uint8Array for proper binary handling,
 * then converted to binary strings for esptool-js compatibility.
 */

// Import esptool-js from unpkg (latest version with Uint8Array support)
import { ESPLoader, Transport } from 'https://unpkg.com/esptool-js/bundle.js';

// ============================================================================
// Configuration
// ============================================================================

const CONFIG = {
  // Flash addresses for ESP32-P4
  flashAddresses: {
    'bootloader.bin': 0x2000,
    'partition-table.bin': 0x8000,
    'ota_data_initial.bin': 0x10000,
    'p3a.bin': 0x20000,
    'storage.bin': 0x1020000,
    'network_adapter.bin': 0x1120000
  },

  // Flash settings required for ESP32-P4
  flashMode: 'dio',
  flashFreq: '80m',
  flashSize: '32MB',

  // Serial settings
  baudrate: 460800,

  // GitHub repository for releases
  githubRepo: 'fabkury/p3a',

  // Required firmware files
  requiredFiles: [
    'bootloader.bin',
    'partition-table.bin',
    'ota_data_initial.bin',
    'p3a.bin',
    'storage.bin',
    'network_adapter.bin'
  ]
};

// ============================================================================
// State
// ============================================================================

let state = {
  transport: null,
  esploader: null,
  connected: false,
  firmware: {}, // { filename: Uint8Array }
  releases: [],
  isFlashing: false
};

// ============================================================================
// DOM Elements
// ============================================================================

const elements = {
  // Browser warning
  browserWarning: document.getElementById('browserWarning'),

  // Step 1: Connect
  connectBtn: document.getElementById('connectBtn'),
  connectBtnText: document.getElementById('connectBtnText'),
  connectionStatus: document.getElementById('connectionStatus'),
  connectionStatusText: document.getElementById('connectionStatusText'),
  connectionInfo: document.getElementById('connectionInfo'),
  chipType: document.getElementById('chipType'),
  macAddress: document.getElementById('macAddress'),
  crystalFreq: document.getElementById('crystalFreq'),
  step1Number: document.getElementById('step1Number'),

  // Step 2: Firmware
  sourceRelease: document.getElementById('sourceRelease'),
  sourceLocal: document.getElementById('sourceLocal'),
  releaseSection: document.getElementById('releaseSection'),
  localSection: document.getElementById('localSection'),
  versionSelect: document.getElementById('versionSelect'),
  downloadFirmwareBtn: document.getElementById('downloadFirmwareBtn'),
  fileDropZone: document.getElementById('fileDropZone'),
  fileInput: document.getElementById('fileInput'),
  firmwareStatus: document.getElementById('firmwareStatus'),
  step2Number: document.getElementById('step2Number'),

  // Firmware status indicators
  statusBootloader: document.getElementById('statusBootloader'),
  statusPartition: document.getElementById('statusPartition'),
  statusOta: document.getElementById('statusOta'),
  statusApp: document.getElementById('statusApp'),
  statusStorage: document.getElementById('statusStorage'),
  statusNetwork: document.getElementById('statusNetwork'),

  // Step 3: Flash
  flashBtn: document.getElementById('flashBtn'),
  eraseBtn: document.getElementById('eraseBtn'),
  progressSection: document.getElementById('progressSection'),
  progressLabel: document.getElementById('progressLabel'),
  progressPercent: document.getElementById('progressPercent'),
  progressBar: document.getElementById('progressBar'),
  successBanner: document.getElementById('successBanner'),
  console: document.getElementById('console'),
  clearConsoleBtn: document.getElementById('clearConsoleBtn'),
  step3Number: document.getElementById('step3Number')
};

// ============================================================================
// Console Logging
// ============================================================================

function log(message, type = 'info') {
  const line = document.createElement('div');
  line.className = `console-line ${type}`;
  
  const timestamp = new Date().toLocaleTimeString('en-US', { 
    hour12: false, 
    hour: '2-digit', 
    minute: '2-digit', 
    second: '2-digit' 
  });
  
  line.innerHTML = `<span class="timestamp">[${timestamp}]</span>${escapeHtml(message)}`;
  elements.console.appendChild(line);
  elements.console.scrollTop = elements.console.scrollHeight;

  // Also log to browser console
  const consoleMethod = type === 'error' ? 'error' : type === 'warning' ? 'warn' : 'log';
  console[consoleMethod](`[p3a-flasher] ${message}`);
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

function clearConsole() {
  elements.console.innerHTML = '';
}

// ============================================================================
// Browser Support Check
// ============================================================================

function checkBrowserSupport() {
  if (!('serial' in navigator)) {
    elements.browserWarning.classList.add('show');
    elements.connectBtn.disabled = true;
    log('Web Serial API not supported. Please use Chrome, Edge, or Opera.', 'error');
    return false;
  }
  return true;
}

// ============================================================================
// Connection Management
// ============================================================================

async function connect() {
  if (state.connected) {
    await disconnect();
    return;
  }

  try {
    elements.connectBtn.disabled = true;
    elements.connectBtnText.textContent = 'Connecting...';
    log('Requesting serial port...');

    // Request port from user
    const port = await navigator.serial.requestPort({
      filters: [
        { usbVendorId: 0x303a }, // Espressif
        { usbVendorId: 0x10c4 }, // Silicon Labs CP210x
        { usbVendorId: 0x1a86 }, // QinHeng CH340
        { usbVendorId: 0x0403 }  // FTDI
      ]
    });

    log('Port selected, initializing transport...');

    // Create transport
    state.transport = new Transport(port, true);

    // Create ESPLoader with terminal output
    const loaderOptions = {
      transport: state.transport,
      baudrate: CONFIG.baudrate,
      terminal: {
        clean: () => {},
        writeLine: (data) => log(data),
        write: (data) => {
          // Handle progress output without creating new lines for each character
          if (data && data.trim()) {
            log(data.trim());
          }
        }
      }
    };

    state.esploader = new ESPLoader(loaderOptions);

    log('Connecting to ESP32...');
    const chip = await state.esploader.main();

    log(`Connected! Chip: ${chip}`, 'success');

    // Get chip info
    const macAddr = await state.esploader.chip.readMac(state.esploader);
    
    state.connected = true;
    updateConnectionUI(true, chip, macAddr);

  } catch (error) {
    log(`Connection failed: ${error.message}`, 'error');
    await disconnect();
  } finally {
    elements.connectBtn.disabled = false;
  }
}

async function disconnect() {
  try {
    if (state.transport) {
      await state.transport.disconnect();
    }
  } catch (e) {
    // Ignore disconnect errors
  }

  state.transport = null;
  state.esploader = null;
  state.connected = false;
  updateConnectionUI(false);
  log('Disconnected');
}

function updateConnectionUI(connected, chip = null, mac = null) {
  if (connected) {
    elements.connectBtnText.textContent = 'Disconnect';
    elements.connectionStatus.classList.add('connected');
    elements.connectionStatusText.textContent = 'Connected';
    elements.connectionInfo.classList.remove('hidden');
    elements.step1Number.classList.add('complete');

    if (chip) {
      elements.chipType.textContent = chip;
    }
    if (mac) {
      elements.macAddress.textContent = formatMac(mac);
    }
    elements.crystalFreq.textContent = '40 MHz';

    // Enable erase button
    elements.eraseBtn.disabled = false;

  } else {
    elements.connectBtnText.textContent = 'Connect';
    elements.connectionStatus.classList.remove('connected');
    elements.connectionStatusText.textContent = 'Disconnected';
    elements.connectionInfo.classList.add('hidden');
    elements.step1Number.classList.remove('complete');

    elements.chipType.textContent = '—';
    elements.macAddress.textContent = '—';
    elements.crystalFreq.textContent = '—';

    // Disable buttons
    elements.eraseBtn.disabled = true;
    updateFlashButton();
  }
}

function formatMac(macArray) {
  if (!macArray) return '—';
  return Array.from(macArray)
    .map(b => b.toString(16).padStart(2, '0'))
    .join(':')
    .toUpperCase();
}

// ============================================================================
// Firmware Source Selection
// ============================================================================

function setupSourceSelection() {
  const radioButtons = document.querySelectorAll('input[name="firmwareSource"]');
  
  radioButtons.forEach(radio => {
    radio.addEventListener('change', (e) => {
      // Update selected state
      elements.sourceRelease.classList.toggle('selected', e.target.value === 'release');
      elements.sourceLocal.classList.toggle('selected', e.target.value === 'local');

      // Show/hide sections
      elements.releaseSection.classList.toggle('hidden', e.target.value !== 'release');
      elements.localSection.classList.toggle('hidden', e.target.value !== 'local');
    });
  });
}

// ============================================================================
// GitHub Releases
// ============================================================================

async function fetchReleases() {
  try {
    log('Fetching releases from GitHub...');
    const response = await fetch(`https://api.github.com/repos/${CONFIG.githubRepo}/releases`);
    
    if (!response.ok) {
      throw new Error(`GitHub API returned ${response.status}`);
    }

    state.releases = await response.json();

    // Filter releases that have firmware assets
    const firmwareReleases = state.releases.filter(release => 
      release.assets.some(asset => asset.name.endsWith('.zip') || asset.name.endsWith('.bin'))
    );

    // Populate select
    elements.versionSelect.innerHTML = '';
    
    if (firmwareReleases.length === 0) {
      elements.versionSelect.innerHTML = '<option value="">No firmware releases found</option>';
      log('No firmware releases found', 'warning');
      return;
    }

    firmwareReleases.forEach((release, index) => {
      const option = document.createElement('option');
      option.value = release.tag_name;
      option.textContent = `${release.tag_name}${index === 0 ? ' (latest)' : ''} — ${release.name || release.tag_name}`;
      elements.versionSelect.appendChild(option);
    });

    elements.downloadFirmwareBtn.disabled = false;
    log(`Found ${firmwareReleases.length} firmware release(s)`, 'success');

  } catch (error) {
    log(`Failed to fetch releases: ${error.message}`, 'error');
    elements.versionSelect.innerHTML = '<option value="">Failed to load releases</option>';
  }
}

async function downloadFirmware() {
  const tagName = elements.versionSelect.value;
  if (!tagName) return;

  const release = state.releases.find(r => r.tag_name === tagName);
  if (!release) {
    log('Release not found', 'error');
    return;
  }

  try {
    elements.downloadFirmwareBtn.disabled = true;
    elements.downloadFirmwareBtn.innerHTML = `
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin">
        <circle cx="12" cy="12" r="10" stroke-dasharray="32" stroke-dashoffset="32"/>
      </svg>
      Downloading...
    `;

    // Look for firmware ZIP
    const firmwareAsset = release.assets.find(asset => 
      asset.name.includes('firmware') && asset.name.endsWith('.zip')
    ) || release.assets.find(asset => asset.name.endsWith('.zip'));

    if (firmwareAsset) {
      log(`Downloading ${firmwareAsset.name}...`);
      await downloadAndExtractZip(firmwareAsset.browser_download_url);
    } else {
      // Try to download individual bin files
      log('No ZIP found, looking for individual .bin files...');
      await downloadIndividualFiles(release.assets);
    }

  } catch (error) {
    log(`Download failed: ${error.message}`, 'error');
  } finally {
    elements.downloadFirmwareBtn.disabled = false;
    elements.downloadFirmwareBtn.innerHTML = `
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4M7 10l5 5 5-5M12 15V3"/>
      </svg>
      Download Firmware
    `;
  }
}

async function downloadAndExtractZip(url) {
  log(`Fetching ZIP from ${url}...`);
  
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Failed to download: ${response.status}`);
  }

  const blob = await response.blob();
  await extractZip(blob);
}

async function extractZip(blob) {
  log('Extracting firmware files...');
  
  const zip = await JSZip.loadAsync(blob);
  
  // Clear existing firmware
  state.firmware = {};

  // Extract required files
  for (const filename of CONFIG.requiredFiles) {
    // Look for the file in the ZIP (may be in a subdirectory)
    let fileEntry = zip.file(filename);
    
    if (!fileEntry) {
      // Search in subdirectories
      const files = zip.file(new RegExp(`${filename}$`));
      if (files.length > 0) {
        fileEntry = files[0];
      }
    }

    if (fileEntry) {
      const data = await fileEntry.async('uint8array');
      state.firmware[filename] = data;
      log(`  ✓ ${filename} (${formatSize(data.length)})`, 'success');
    } else {
      log(`  ✗ ${filename} not found in ZIP`, 'warning');
    }
  }

  updateFirmwareStatus();
}

async function downloadIndividualFiles(assets) {
  state.firmware = {};

  for (const filename of CONFIG.requiredFiles) {
    const asset = assets.find(a => a.name === filename);
    if (asset) {
      log(`Downloading ${filename}...`);
      const response = await fetch(asset.browser_download_url);
      if (response.ok) {
        const buffer = await response.arrayBuffer();
        state.firmware[filename] = new Uint8Array(buffer);
        log(`  ✓ ${filename} (${formatSize(state.firmware[filename].length)})`, 'success');
      }
    }
  }

  updateFirmwareStatus();
}

// ============================================================================
// Local File Handling
// ============================================================================

function setupFileHandling() {
  // Click to upload
  elements.fileDropZone.addEventListener('click', () => {
    elements.fileInput.click();
  });

  // File input change
  elements.fileInput.addEventListener('change', (e) => {
    handleFiles(e.target.files);
  });

  // Drag and drop
  elements.fileDropZone.addEventListener('dragover', (e) => {
    e.preventDefault();
    elements.fileDropZone.classList.add('dragover');
  });

  elements.fileDropZone.addEventListener('dragleave', () => {
    elements.fileDropZone.classList.remove('dragover');
  });

  elements.fileDropZone.addEventListener('drop', (e) => {
    e.preventDefault();
    elements.fileDropZone.classList.remove('dragover');
    handleFiles(e.dataTransfer.files);
  });
}

async function handleFiles(files) {
  for (const file of files) {
    if (file.name.endsWith('.zip')) {
      log(`Processing ZIP: ${file.name}`);
      await extractZip(file);
    } else if (file.name.endsWith('.bin')) {
      log(`Loading: ${file.name}`);
      const buffer = await file.arrayBuffer();
      state.firmware[file.name] = new Uint8Array(buffer);
      log(`  ✓ ${file.name} (${formatSize(state.firmware[file.name].length)})`, 'success');
    }
  }

  updateFirmwareStatus();
}

// ============================================================================
// Firmware Status
// ============================================================================

function updateFirmwareStatus() {
  elements.firmwareStatus.classList.remove('hidden');

  const statusMap = {
    'bootloader.bin': elements.statusBootloader,
    'partition-table.bin': elements.statusPartition,
    'ota_data_initial.bin': elements.statusOta,
    'p3a.bin': elements.statusApp,
    'storage.bin': elements.statusStorage,
    'network_adapter.bin': elements.statusNetwork
  };

  let allLoaded = true;

  for (const [filename, element] of Object.entries(statusMap)) {
    if (state.firmware[filename]) {
      element.textContent = formatSize(state.firmware[filename].length);
      element.classList.add('loaded');
    } else {
      element.textContent = 'Not loaded';
      element.classList.remove('loaded');
      allLoaded = false;
    }
  }

  // Update step indicator
  if (allLoaded) {
    elements.step2Number.classList.add('complete');
  } else {
    elements.step2Number.classList.remove('complete');
  }

  updateFlashButton();
}

function updateFlashButton() {
  const allFilesLoaded = CONFIG.requiredFiles.every(f => state.firmware[f]);
  elements.flashBtn.disabled = !state.connected || !allFilesLoaded || state.isFlashing;
}

// ============================================================================
// Flashing
// ============================================================================

async function flash() {
  if (!state.connected || !state.esploader) {
    log('Not connected', 'error');
    return;
  }

  const allFilesLoaded = CONFIG.requiredFiles.every(f => state.firmware[f]);
  if (!allFilesLoaded) {
    log('Not all firmware files are loaded', 'error');
    return;
  }

  state.isFlashing = true;
  elements.flashBtn.disabled = true;
  elements.eraseBtn.disabled = true;
  elements.progressSection.classList.remove('hidden');
  elements.successBanner.classList.remove('hidden');
  elements.successBanner.classList.remove('show');
  elements.progressBar.classList.remove('error');

  try {
    log('Starting flash process...');
    log(`Flash settings: mode=${CONFIG.flashMode}, freq=${CONFIG.flashFreq}, size=${CONFIG.flashSize}`);

    // Prepare file array - convert Uint8Array to binary string for esptool-js
    // Note: esptool-js currently expects binary strings, not Uint8Array
    const fileArray = CONFIG.requiredFiles.map(filename => ({
      data: uint8ArrayToBinaryString(state.firmware[filename]),
      address: CONFIG.flashAddresses[filename]
    }));
    
    log('Firmware data converted to binary format');

    log(`Flashing ${fileArray.length} files...`);

    // Calculate total size for progress
    const totalSize = fileArray.reduce((sum, f) => sum + f.data.length, 0);
    let writtenSize = 0;

    // Flash with proper settings
    const flashOptions = {
      fileArray: fileArray,
      flashSize: CONFIG.flashSize,
      flashMode: CONFIG.flashMode,
      flashFreq: CONFIG.flashFreq,
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        // Calculate overall progress
        let currentFileOffset = 0;
        for (let i = 0; i < fileIndex; i++) {
          currentFileOffset += fileArray[i].data.length;
        }
        const overallWritten = currentFileOffset + written;
        const percent = Math.round((overallWritten / totalSize) * 100);
        
        updateProgress(
          `Writing ${CONFIG.requiredFiles[fileIndex]}...`,
          percent
        );
      }
    };

    await state.esploader.writeFlash(flashOptions);

    log('Flash complete!', 'success');
    updateProgress('Complete!', 100);
    elements.successBanner.classList.add('show');
    elements.step3Number.classList.add('complete');

    // Hard reset the device
    log('Resetting device...');
    await state.esploader.hardReset();

  } catch (error) {
    log(`Flash failed: ${error.message}`, 'error');
    elements.progressBar.classList.add('error');
    updateProgress('Flash failed', 0);
  } finally {
    state.isFlashing = false;
    elements.flashBtn.disabled = false;
    elements.eraseBtn.disabled = !state.connected;
  }
}

async function eraseFlash() {
  if (!state.connected || !state.esploader) {
    log('Not connected', 'error');
    return;
  }

  if (!confirm('This will erase ALL data on the flash, including any saved settings. Continue?')) {
    return;
  }

  state.isFlashing = true;
  elements.flashBtn.disabled = true;
  elements.eraseBtn.disabled = true;
  elements.progressSection.classList.remove('hidden');

  try {
    log('Erasing flash... (this may take a minute)');
    updateProgress('Erasing...', 50);

    await state.esploader.eraseFlash();

    log('Erase complete!', 'success');
    updateProgress('Erase complete', 100);

  } catch (error) {
    log(`Erase failed: ${error.message}`, 'error');
  } finally {
    state.isFlashing = false;
    updateFlashButton();
    elements.eraseBtn.disabled = !state.connected;
  }
}

function updateProgress(label, percent) {
  elements.progressLabel.textContent = label;
  elements.progressPercent.textContent = `${percent}%`;
  elements.progressBar.style.width = `${percent}%`;
}

// ============================================================================
// Utilities
// ============================================================================

function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

/**
 * Convert Uint8Array to binary string for esptool-js compatibility.
 * esptool-js currently expects binary strings where each character
 * represents one byte (charCode 0-255).
 * 
 * Uses chunked processing to avoid call stack overflow on large files.
 */
function uint8ArrayToBinaryString(uint8Array) {
  const chunkSize = 0x8000; // 32KB chunks to avoid stack overflow
  const chunks = [];
  
  for (let i = 0; i < uint8Array.length; i += chunkSize) {
    const chunk = uint8Array.subarray(i, Math.min(i + chunkSize, uint8Array.length));
    chunks.push(String.fromCharCode.apply(null, chunk));
  }
  
  return chunks.join('');
}

// ============================================================================
// Initialization
// ============================================================================

function init() {
  log('p3a Web Flasher initialized');
  log('esptool-js loaded (latest version with Uint8Array support)');

  // Check browser support
  if (!checkBrowserSupport()) {
    return;
  }

  // Setup event listeners
  elements.connectBtn.addEventListener('click', connect);
  elements.downloadFirmwareBtn.addEventListener('click', downloadFirmware);
  elements.flashBtn.addEventListener('click', flash);
  elements.eraseBtn.addEventListener('click', eraseFlash);
  elements.clearConsoleBtn.addEventListener('click', clearConsole);

  // Setup firmware source selection
  setupSourceSelection();

  // Setup file handling
  setupFileHandling();

  // Fetch releases
  fetchReleases();

  log('Ready. Click "Connect" to begin.');
}

// Start when DOM is ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', init);
} else {
  init();
}

