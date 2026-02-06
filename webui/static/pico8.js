// PICO-8 Monitor - Browser-side JavaScript

const PICO_WIDTH = 128;
const PICO_HEIGHT = 128;
const PICO_PALETTE_SIZE = 16;

// PICO-8 palette (RGB888)
const PICO8_PALETTE = [
    [0x00, 0x00, 0x00], [0x1D, 0x2B, 0x53], [0x7E, 0x25, 0x53], [0x00, 0x87, 0x51],
    [0xAB, 0x52, 0x36], [0x5F, 0x57, 0x4F], [0xC2, 0xC3, 0xC7], [0xFF, 0xF1, 0xE8],
    [0xFF, 0x00, 0x4D], [0xFF, 0xA3, 0x00], [0xFF, 0xEC, 0x27], [0x00, 0xE4, 0x36],
    [0x29, 0xAD, 0xFF], [0x83, 0x76, 0x9C], [0xFF, 0x77, 0xA8], [0xFF, 0xCC, 0xAA],
];

let Module = null;
let ws = null;
let animationFrameId = null;
let isRunning = false;
let frameCount = 0;
let lastFpsTime = 0;
let currentFps = 0;
let palettePtr = 0;
let paletteView = null;
let currentPalette = PICO8_PALETTE.map(color => color.slice());

// Adaptive frame rate control
const MIN_SKIP = 2;          // Maximum 30fps send rate
const MAX_SKIP = 12;         // Minimum ~5fps send rate
const FRAME_PACKET_SIZE = 8246;
let streamSkipCounter = 0;
let streamSkipInterval = 2;  // Start at 30fps (send every 2nd frame)
let streamFrameCount = 0;

// Application-level RTT measurement
let rttMs = 20;
let pingSentAt = 0;
let rttHistory = [];
let pingIntervalId = null;

// DOM elements
const canvas = document.getElementById('pico-canvas');
const ctx = canvas.getContext('2d');
const fileInput = document.getElementById('file-input');
const urlInput = document.getElementById('url-input');
const loadUrlBtn = document.getElementById('load-url-btn');
const fileName = document.getElementById('file-name');
const status = document.getElementById('status');
const fpsDisplay = document.getElementById('fps');
const errorMessage = document.getElementById('error-message');

// Initialize WASM module
async function initWasm() {
    try {
        updateStatus('Loading PICO-8 runtime...', 'info');
        
        // Wait for Fake08Module to be available (loaded by script tag)
        if (typeof Fake08Module === 'undefined') {
            throw new Error('Fake08Module not found. Make sure fake08.js is loaded.');
        }

        Module = await Fake08Module({
            locateFile: (path) => {
                if (path.endsWith('.wasm')) {
                    return '/static/fake08.wasm';
                }
                return path;
            }
        });

        // Create /p8carts directory in Emscripten FS
        try {
            Module.FS.mkdir('/p8carts');
        } catch (e) {
            // Directory might already exist
        }

        // Create /fake08 directory for logs
        try {
            Module.FS.mkdir('/fake08');
        } catch (e) {
            // Directory might already exist
        }

        // Initialize fake-08
        Module._f08_init();

        if (!palettePtr) {
            palettePtr = Module._malloc(PICO_PALETTE_SIZE * 4);
            paletteView = new Uint8Array(Module.HEAPU8.buffer, palettePtr, PICO_PALETTE_SIZE * 4);
        }

        updateStatus('Ready. Load a cart to start.', 'success');
        return true;
    } catch (error) {
        console.error('initWasm error:', error);
        updateStatus('Failed to load PICO-8 runtime: ' + error.message, 'error');
        showError(error.message);
        return false;
    }
}

// Update status message
function updateStatus(message, type = 'info') {
    status.textContent = message;
    status.className = 'status ' + type;
}

// Show error message
function showError(message) {
    errorMessage.textContent = message;
    errorMessage.classList.add('show');
    setTimeout(() => {
        errorMessage.classList.remove('show');
    }, 5000);
}

function getLastCartError() {
    if (!Module || typeof Module._f08_get_last_error !== 'function' || typeof Module.UTF8ToString !== 'function') {
        return null;
    }
    try {
        const ptr = Module._f08_get_last_error();
        if (!ptr) {
            return null;
        }
        const message = Module.UTF8ToString(ptr);
        if (typeof Module._free === 'function') {
            Module._free(ptr);
        }
        return message;
    } catch (err) {
        return null;
    }
}

function formatCartError(message) {
    const detail = getLastCartError();
    if (detail && !message.includes(detail)) {
        return `${message}: ${detail}`;
    }
    return message;
}

// Load cart from local file
async function loadLocalCart(file) {
    try {
        updateStatus('Loading cart...', 'info');
        fileName.textContent = 'Loading: ' + file.name;

        const arrayBuffer = await file.arrayBuffer();
        const uint8Array = new Uint8Array(arrayBuffer);
        const name = file.name;

        // Write to Emscripten FS
        Module.FS.writeFile(`/p8carts/${name}`, uint8Array);

        // Load cart
        let result = Module._f08_load_cart(`/p8carts/${name}`);

        if (result !== 0) {
            // Fallback to memory-based loading
            const dataPtr = Module._malloc(uint8Array.length);
            Module.HEAPU8.set(uint8Array, dataPtr);
            result = Module._f08_load_cart_data(dataPtr, uint8Array.length);
            Module._free(dataPtr);
        }
        
        if (result !== 0) {
            const detail = getLastCartError();
            const errorMsg = detail || 'Failed to load cart';
            throw new Error(errorMsg);
        }

        fileName.textContent = 'Loaded: ' + name;
        updateStatus('Cart loaded successfully', 'success');
        startEmulation();
    } catch (error) {
        const msg = formatCartError(error.message);
        updateStatus('Failed to load cart: ' + msg, 'error');
        showError(msg);
        fileName.textContent = '';
    }
}

// Load cart from URL
async function loadCartFromUrl(url) {
    try {
        updateStatus('Downloading cart...', 'info');
        fileName.textContent = 'Downloading from URL...';

        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}: ${response.statusText}`);
        }

        const arrayBuffer = await response.arrayBuffer();
        const uint8Array = new Uint8Array(arrayBuffer);
        
        // Extract filename from URL
        const urlParts = url.split('/');
        const urlFilename = urlParts[urlParts.length - 1].split('?')[0] || 'remote_cart.p8.png';
        const name = urlFilename;

        // Write to Emscripten FS
        Module.FS.writeFile(`/p8carts/${name}`, uint8Array);

        // Try loading as file path first
        let result = Module._f08_load_cart(`/p8carts/${name}`);
        
        // If that fails, try loading directly from memory
        if (result !== 0) {
            const dataPtr = Module._malloc(uint8Array.length);
            Module.HEAPU8.set(uint8Array, dataPtr);
            result = Module._f08_load_cart_data(dataPtr, uint8Array.length);
            Module._free(dataPtr);
        }

        if (result !== 0) {
            const detail = getLastCartError();
            throw new Error(detail || 'Failed to load cart data');
        }

        fileName.textContent = 'Loaded: ' + name;
        updateStatus('Cart loaded successfully', 'success');
        startEmulation();
    } catch (error) {
        const msg = formatCartError(error.message);
        updateStatus('Failed to load cart: ' + msg, 'error');
        showError(msg);
        fileName.textContent = '';
    }
}

// Connect WebSocket
function connectWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/pico_stream`;
    
    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    
    ws.onopen = () => {
        updateStatus('Connected to ESP32', 'success');
        // Start application-level ping interval for RTT measurement
        pingIntervalId = setInterval(() => {
            if (pingSentAt === 0 && ws && ws.readyState === WebSocket.OPEN) {
                const ping = new Uint8Array([0x70, 0x38, 0x50]); // "p8P"
                ws.send(ping.buffer);
                pingSentAt = performance.now();
            } else if (pingSentAt !== 0 && performance.now() - pingSentAt > 2000) {
                // Ping timeout: push penalty RTT and reset
                rttHistory.push(500);
                if (rttHistory.length > 6) rttHistory.shift();
                pingSentAt = 0;
            }
        }, 500);
    };

    ws.onmessage = (event) => {
        if (event.data instanceof ArrayBuffer && event.data.byteLength === 3) {
            const bytes = new Uint8Array(event.data);
            if (bytes[0] === 0x70 && bytes[1] === 0x38 && bytes[2] === 0x51) { // "p8Q"
                rttMs = performance.now() - pingSentAt;
                rttHistory.push(rttMs);
                if (rttHistory.length > 6) rttHistory.shift();
                pingSentAt = 0;
            }
        }
    };
    
    ws.onerror = (error) => {
        updateStatus('WebSocket error', 'error');
        console.error('WebSocket error:', error);
    };
    
    ws.onclose = () => {
        updateStatus('Disconnected from ESP32', 'info');
        if (pingIntervalId) {
            clearInterval(pingIntervalId);
            pingIntervalId = null;
        }
        ws = null;
    };
}

// Start emulation loop
function startEmulation() {
    if (isRunning) {
        return;
    }

    isRunning = true;
    connectWebSocket();
    
    // Get framebuffer pointer
    const fbPtr = Module._f08_get_framebuffer_ptr();
    if (!fbPtr) {
        updateStatus('Failed to get framebuffer', 'error');
        isRunning = false;
        return;
    }

    const fbView = new Uint8Array(Module.HEAPU8.buffer, fbPtr, PICO_WIDTH * PICO_HEIGHT / 2);

    // Initialize input state
    let kdown = 0;
    let kheld = 0;
    let mouseX = 0;
    let mouseY = 0;
    let mouseBtnState = 0;

    // Keyboard input handling
    const keyMap = {
        'ArrowLeft': 0x01,  // P8_KEY_LEFT
        'ArrowRight': 0x02, // P8_KEY_RIGHT
        'ArrowUp': 0x04,    // P8_KEY_UP
        'ArrowDown': 0x08,  // P8_KEY_DOWN
        'KeyZ': 0x10,       // P8_KEY_X
        'KeyX': 0x20,       // P8_KEY_O
    };

    const keysDown = new Set();

    document.addEventListener('keydown', (e) => {
        if (keyMap[e.code]) {
            e.preventDefault();
            const bit = keyMap[e.code];
            kdown |= bit;
            kheld |= bit;
            keysDown.add(e.code);
        }
    });

    document.addEventListener('keyup', (e) => {
        if (keyMap[e.code]) {
            e.preventDefault();
            const bit = keyMap[e.code];
            kheld &= ~bit;
            keysDown.delete(e.code);
        }
    });

    // Mouse input handling
    canvas.addEventListener('mousemove', (e) => {
        const rect = canvas.getBoundingClientRect();
        const scaleX = canvas.width / rect.width;
        const scaleY = canvas.height / rect.height;
        mouseX = Math.floor((e.clientX - rect.left) * scaleX);
        mouseY = Math.floor((e.clientY - rect.top) * scaleY);
    });

    canvas.addEventListener('mousedown', (e) => {
        e.preventDefault();
        if (e.button === 0) mouseBtnState |= 0x01; // Left
        if (e.button === 1) mouseBtnState |= 0x04; // Middle
        if (e.button === 2) mouseBtnState |= 0x02; // Right
    });

    canvas.addEventListener('mouseup', (e) => {
        e.preventDefault();
        if (e.button === 0) mouseBtnState &= ~0x01;
        if (e.button === 1) mouseBtnState &= ~0x04;
        if (e.button === 2) mouseBtnState &= ~0x02;
    });

    canvas.addEventListener('contextmenu', (e) => {
        e.preventDefault();
    });

    function refreshPalette() {
        if (!palettePtr || typeof Module._f08_get_palette_rgba !== 'function') {
            return;
        }
        if (!paletteView || paletteView.buffer !== Module.HEAPU8.buffer) {
            paletteView = new Uint8Array(Module.HEAPU8.buffer, palettePtr, PICO_PALETTE_SIZE * 4);
        }
        Module._f08_get_palette_rgba(palettePtr);
        for (let i = 0; i < PICO_PALETTE_SIZE; i++) {
            const base = i * 4;
            currentPalette[i][0] = paletteView[base + 0];
            currentPalette[i][1] = paletteView[base + 1];
            currentPalette[i][2] = paletteView[base + 2];
        }
    }

    // Animation loop
    function animate() {
        if (!isRunning) {
            return;
        }

        // Update input state
        Module._f08_set_inputs(kdown, kheld, mouseX, mouseY, mouseBtnState);
        kdown = 0; // Clear kdown after use (it's edge-triggered)

        // Step frame
        Module._f08_step_frame();
        refreshPalette();

        // Read framebuffer
        const imageData = ctx.createImageData(PICO_WIDTH, PICO_HEIGHT);
        
        for (let y = 0; y < PICO_HEIGHT; y++) {
            for (let x = 0; x < PICO_WIDTH; x++) {
                const idx = Math.floor((y * PICO_WIDTH + x) / 2);
                const byte = fbView[idx];
                const nibble = (x & 1) ? (byte >> 4) : (byte & 0x0F);
                
                // Get palette index (use palette map if available)
                let paletteIdx = nibble & 0x0F;
                const color = currentPalette[paletteIdx] || PICO8_PALETTE[paletteIdx];
                const pixelIdx = (y * PICO_WIDTH + x) * 4;
                imageData.data[pixelIdx + 0] = color[0]; // R
                imageData.data[pixelIdx + 1] = color[1]; // G
                imageData.data[pixelIdx + 2] = color[2]; // B
                imageData.data[pixelIdx + 3] = 255;      // A
            }
        }

        // Draw to canvas
        ctx.putImageData(imageData, 0, 0);

        // Stream to ESP32 via WebSocket with adaptive rate control
        streamSkipCounter++;
        if (streamSkipCounter >= streamSkipInterval && ws && ws.readyState === WebSocket.OPEN) {
            computeAdaptiveRate();
            streamFrame(fbView, currentPalette);
            streamSkipCounter = 0;
        }

        // Update FPS
        frameCount++;
        const now = performance.now();
        if (now - lastFpsTime >= 1000) {
            currentFps = frameCount;
            const streamFps = streamFrameCount;
            frameCount = 0;
            streamFrameCount = 0;
            lastFpsTime = now;
            fpsDisplay.textContent = `FPS: ${currentFps} | Stream: ${streamFps}`;
        }

        animationFrameId = requestAnimationFrame(animate);
    }

    animate();
}

// Stream frame to ESP32
function streamFrame(fbView, palette) {
    // Packet format: [magic:3][payload_len:2][flags:1][palette?:48][framebuffer:8192]
    const PACKET_HEADER_SIZE = 6; // 3 magic + 2 len + 1 flags
    const PALETTE_SIZE = 48; // 16 colors * 3 bytes RGB
    const FRAMEBUFFER_SIZE = PICO_WIDTH * PICO_HEIGHT / 2; // 8192 bytes
    
    // For now, send palette every frame (can optimize later)
    const hasPalette = true;
    const payloadLen = (hasPalette ? PALETTE_SIZE : 0) + FRAMEBUFFER_SIZE;
    
    const packet = new Uint8Array(PACKET_HEADER_SIZE + payloadLen);
    
    // Header (little-endian length)
    packet[0] = 0x70; // 'p'
    packet[1] = 0x38; // '8'
    packet[2] = 0x46; // 'F' (p8F)
    packet[3] = payloadLen & 0xFF;
    packet[4] = (payloadLen >> 8) & 0xFF;
    packet[5] = hasPalette ? 0x01 : 0x00; // flags

    let offset = PACKET_HEADER_SIZE;
    
    // Palette (RGB888, 16 colors)
    if (hasPalette) {
        for (let i = 0; i < PICO_PALETTE_SIZE; i++) {
            const color = palette[i] || PICO8_PALETTE[i];
            packet[offset++] = color[0]; // R
            packet[offset++] = color[1]; // G
            packet[offset++] = color[2]; // B
        }
    }
    
    // Framebuffer
    packet.set(fbView, offset);

    ws.send(packet.buffer);
    streamFrameCount++;
}

// Compute adaptive streaming rate based on buffer pressure and RTT
function computeAdaptiveRate() {
    const bufferFrames = ws.bufferedAmount / FRAME_PACKET_SIZE;
    let bufferPressure = 0;
    if (bufferFrames > 2.5) bufferPressure = 2;
    else if (bufferFrames > 1.0) bufferPressure = 1;

    const avgRtt = rttHistory.length > 0
        ? rttHistory.reduce((a, b) => a + b, 0) / rttHistory.length
        : rttMs;
    let rttPressure = 0;
    if (avgRtt > 150) rttPressure = 3;
    else if (avgRtt > 80) rttPressure = 1;
    else if (avgRtt < 25) rttPressure = -1;

    const pressure = bufferPressure + rttPressure;
    if (pressure >= 2) {
        streamSkipInterval = Math.min(MAX_SKIP, streamSkipInterval + Math.ceil(pressure / 2));
    } else if (pressure <= -1) {
        streamSkipInterval = Math.max(MIN_SKIP, streamSkipInterval - 1);
    }
}

// Signal exit to ESP32 (fire-and-forget)
function signalExit() {
    try {
        // Use sendBeacon for reliable delivery during page unload
        navigator.sendBeacon('/pico8/exit');
    } catch (e) {
        // Fallback to sync XHR if sendBeacon unavailable
        try {
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/pico8/exit', false); // sync
            xhr.send();
        } catch (e2) {
            // Ignore errors during unload
        }
    }
}

// Stop emulation
function stopEmulation() {
    isRunning = false;
    if (animationFrameId) {
        cancelAnimationFrame(animationFrameId);
        animationFrameId = null;
    }
    if (pingIntervalId) {
        clearInterval(pingIntervalId);
        pingIntervalId = null;
    }
    if (ws) {
        ws.close();
        ws = null;
    }
    streamSkipInterval = 2;
    streamSkipCounter = 0;
    rttHistory = [];
    rttMs = 20;
    pingSentAt = 0;
    signalExit();
}

// Event listeners
fileInput.addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (file && Module) {
        loadLocalCart(file);
    }
});

loadUrlBtn.addEventListener('click', () => {
    const url = urlInput.value.trim();
    if (url && Module) {
        loadCartFromUrl(url);
    }
});

urlInput.addEventListener('keypress', (e) => {
    if (e.key === 'Enter' && Module) {
        loadCartFromUrl(urlInput.value.trim());
    }
});

// Initialize on page load
window.addEventListener('load', () => {
    initWasm();
});

// Cleanup on page unload
window.addEventListener('beforeunload', () => {
    stopEmulation();
});

// More reliable unload event for mobile browsers
window.addEventListener('pagehide', () => {
    signalExit();
});

// Intercept navigation link clicks to signal exit immediately
document.addEventListener('click', (e) => {
    const link = e.target.closest('a[href]');
    if (link && link.href && !link.href.startsWith('javascript:')) {
        // Check if it's an internal navigation (not external link)
        const url = new URL(link.href, window.location.origin);
        if (url.origin === window.location.origin) {
            signalExit();
        }
    }
});

