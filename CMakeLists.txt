# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# =============================================================================
# Firmware Version
# =============================================================================
# This version is embedded in the firmware binary and used by OTA updates.
# Format: MAJOR.MINOR.PATCH (semantic versioning)
# - Increment MAJOR for breaking changes
# - Increment MINOR for new features (backward compatible)
# - Increment PATCH for bug fixes
# For testing OTA, use pre-release versions on GitHub
set(PROJECT_VER "0.8.1-dev")
# =============================================================================

# =============================================================================
# Web UI Version (Independent from firmware)
# =============================================================================
# Web UI version uses X.Y format (not X.Y.Z). Updates are independent of
# firmware updates and are checked/installed automatically during OTA checks.
set(WEBUI_VERSION "1.1")
# =============================================================================

# =============================================================================
# API Version (Breaking API changes only)
# =============================================================================
# Bump ONLY when making breaking changes to HTTP API endpoints.
# New firmware MUST remain backwards-compatible with old Web UI at same api_version.
set(P3A_API_VERSION 2)
# =============================================================================

# =============================================================================
# Flasher Tool Build (Windows executable with embedded firmware)
# =============================================================================
# Set P3A_BUILD_FLASHER to OFF to skip flasher build (faster dev builds)
# Set to ON for release builds that need the flasher executable
set(P3A_BUILD_FLASHER ON)
# =============================================================================

# Parse PROJECT_VER to extract major, minor, patch components
# Handle versions with suffixes like "-dev", "-alpha", etc.
string(REGEX REPLACE "^([0-9]+)\\.[0-9]+\\.[0-9]+.*" "\\1" FW_VERSION_MAJOR "${PROJECT_VER}")
string(REGEX REPLACE "^[0-9]+\\.([0-9]+)\\.[0-9]+.*" "\\1" FW_VERSION_MINOR "${PROJECT_VER}")
string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+).*" "\\1" FW_VERSION_PATCH "${PROJECT_VER}")

# Store in cache so components can access them
set(PROJECT_VER "${PROJECT_VER}" CACHE STRING "Project version" FORCE)
set(FW_VERSION_MAJOR "${FW_VERSION_MAJOR}" CACHE STRING "Firmware major version" FORCE)
set(FW_VERSION_MINOR "${FW_VERSION_MINOR}" CACHE STRING "Firmware minor version" FORCE)
set(FW_VERSION_PATCH "${FW_VERSION_PATCH}" CACHE STRING "Firmware patch version" FORCE)
set(P3A_API_VERSION "${P3A_API_VERSION}" CACHE STRING "API version" FORCE)

set(RELEASE_SUBDIR "release/v${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifiers)
project(p3a)

# =============================================================================
# Generate version.txt for Web UI OTA
# =============================================================================
# Write the WEBUI_VERSION to webui/version.txt so it's included in the LittleFS image
# This allows runtime detection of the current web UI version
file(WRITE "${CMAKE_SOURCE_DIR}/webui/version.txt" "${WEBUI_VERSION}")
message(STATUS "Generated webui/version.txt with version ${WEBUI_VERSION}")

# Generate metadata.json for Web UI compatibility checking
file(WRITE "${CMAKE_SOURCE_DIR}/webui/metadata.json"
    "{\"requires_api_version\":${P3A_API_VERSION},\"version\":\"${WEBUI_VERSION}\"}\n")
message(STATUS "Generated webui/metadata.json (requires_api_version: ${P3A_API_VERSION})")

# Create LittleFS image from webui directory
# When PICO-8 is disabled, filter out PICO-8 related assets to reduce binary size
if(CONFIG_P3A_PICO8_ENABLE)
    # PICO-8 enabled: use full webui directory
    littlefs_create_partition_image(storage webui FLASH_IN_PROJECT)
else()
    # PICO-8 disabled: create filtered webui without PICO-8 assets
    set(WEBUI_STAGING_DIR "${CMAKE_BINARY_DIR}/webui_filtered")

    # Clean and recreate staging directory
    file(REMOVE_RECURSE "${WEBUI_STAGING_DIR}")
    file(MAKE_DIRECTORY "${WEBUI_STAGING_DIR}")
    file(MAKE_DIRECTORY "${WEBUI_STAGING_DIR}/static")

    # Copy static files excluding PICO-8 related assets
    file(GLOB STATIC_FILES "${CMAKE_SOURCE_DIR}/webui/static/*")
    foreach(STATIC_FILE ${STATIC_FILES})
        get_filename_component(FILENAME ${STATIC_FILE} NAME)
        # Exclude PICO-8 related files
        if(NOT FILENAME MATCHES "^(fake08\\.|pico8\\.)")
            file(COPY ${STATIC_FILE} DESTINATION "${WEBUI_STAGING_DIR}/static")
        endif()
    endforeach()

    # Do not copy pico8/ directory at all

    # Copy root-level files (index.html, settings.html, ota.html, favicon.ico, version.txt)
    file(GLOB ROOT_FILES "${CMAKE_SOURCE_DIR}/webui/*.html" "${CMAKE_SOURCE_DIR}/webui/*.ico" "${CMAKE_SOURCE_DIR}/webui/*.txt")
    file(COPY ${ROOT_FILES} DESTINATION "${WEBUI_STAGING_DIR}")

    # Copy config directory
    file(COPY "${CMAKE_SOURCE_DIR}/webui/config" DESTINATION "${WEBUI_STAGING_DIR}")

    littlefs_create_partition_image(storage "${WEBUI_STAGING_DIR}" FLASH_IN_PROJECT)
endif()

# =============================================================================
# Post-Build: Copy binaries to release directory and generate SHA256
# =============================================================================
# This creates a helper script that runs after the build to:
# 1. Copy binaries to release/v{version}/ (or /dev/ for dev builds) in project root
# 2. Generate .sha256 checksum files
# Note: Release folder is outside build/ so it persists across clean builds

# Determine if this is a dev build (contains -dev, -alpha, -beta, -rc, -test)
string(REGEX MATCH "-(dev|alpha|beta|rc|test)" IS_DEV_BUILD "${PROJECT_VER}")

# Create release helper script at configure time
file(WRITE "${CMAKE_BINARY_DIR}/create_release.py" "
#!/usr/bin/env python3
import os
import sys
import hashlib
import shutil

build_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
project_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(build_dir)
release_subdir = '${RELEASE_SUBDIR}'
is_dev_build = '${IS_DEV_BUILD}' != ''  # True if version contains -dev, -alpha, etc.
release_dir = os.path.join(project_dir, release_subdir)
version = '${PROJECT_VER}'
webui_version = '${WEBUI_VERSION}'
api_version = ${P3A_API_VERSION}

# Create release directory
os.makedirs(release_dir, exist_ok=True)

# Files to copy (source relative to build_dir or project_dir, destination name, flash address, force flag for non-P4 binaries)
files = [
    ('build:p3a.bin', 'p3a.bin', '0x20000', False),
    ('build:bootloader/bootloader.bin', 'bootloader.bin', '0x2000', False),
    ('build:partition_table/partition-table.bin', 'partition-table.bin', '0x8000', False),
    ('build:ota_data_initial.bin', 'ota_data_initial.bin', '0x10000', False),
    ('build:storage.bin', 'storage.bin', '0x1020000', False),
    ('project:components/slave_ota/firmware/network_adapter.bin', 'network_adapter.bin', '0x1420000', True),
]

print(f'\\n========== Creating Release: {version} ==========')
print(f'Output directory: {release_dir}\\n')

flash_entries = []
has_force_files = False

for src_spec, dst_name, flash_addr, needs_force in files:
    # Parse source specification
    src_type, src_rel = src_spec.split(':', 1)
    if src_type == 'build':
        src = os.path.join(build_dir, src_rel)
    else:  # project
        src = os.path.join(project_dir, src_rel)
    
    dst = os.path.join(release_dir, dst_name)
    
    if not os.path.exists(src):
        print(f'  SKIP: {src_rel} (not found)')
        continue
    
    # Copy file
    shutil.copy2(src, dst)
    
    # Calculate SHA256
    sha256 = hashlib.sha256()
    with open(dst, 'rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            sha256.update(chunk)
    
    hash_hex = sha256.hexdigest()
    
    # Write .sha256 file (just the hash, no filename)
    with open(dst + '.sha256', 'w') as f:
        f.write(hash_hex)
    
    size_kb = os.path.getsize(dst) / 1024
    print(f'  OK: {dst_name} ({size_kb:.1f} KB)')
    print(f'      SHA256: {hash_hex[:16]}...')
    
    # Add to flash entries
    flash_entries.append((flash_addr, dst_name))
    if needs_force:
        has_force_files = True

# Generate flash_args file (address/filename pairs only, NO flash options)
# Flash options must be passed on command line because @flash_args is parsed AFTER write-flash
flash_args_path = os.path.join(release_dir, 'flash_args')
with open(flash_args_path, 'w') as f:
    for addr, name in flash_entries:
        f.write(f'{addr} {name}\\n')

print(f'  OK: flash_args')

# Generate flash command helper
flash_cmd_path = os.path.join(release_dir, 'flash_command.txt')
force_flag = ' --force' if has_force_files else ''
flash_opts = '--flash-mode dio --flash-freq 80m --flash-size 32MB'
with open(flash_cmd_path, 'w') as f:
    f.write('# Flash command for ESP32-P4\\n')
    f.write('# Run from this directory:\\n')
    f.write('\\n')
    f.write('# Windows Command Prompt:\\n')
    f.write(f'python -m esptool --chip esp32p4 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args\\n')
    f.write('\\n')
    f.write('# Windows PowerShell:\\n')
    f.write(f'python -m esptool --chip esp32p4 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} \"@flash_args\"\\n')
    f.write('\\n')
    f.write('# Linux/macOS:\\n')
    f.write(f'python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args\\n')
    f.write('\\n')
    f.write('\\n')
    f.write('# Or with explicit port (replace COM5 or /dev/ttyUSB0 with your port)\\n')
    f.write(f'python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args\\n')

print(f'  OK: flash_command.txt')

# Generate flash_command_app_only.txt for dev builds (flashes only the main app partition)
flash_opts = '--flash_mode dio --flash_freq 80m --flash_size 32MB'
if is_dev_build:
    flash_cmd_app_only_path = os.path.join(release_dir, 'flash_command_app_only.txt')
    # App-only flash: just p3a.bin at 0x20000
    with open(flash_cmd_app_only_path, 'w') as f:
        f.write('# Flash command for ESP32-P4 (APP ONLY - for faster dev builds)\\n')
        f.write('# Only flashes the main application partition, not bootloader/partitions/storage\\n')
        f.write('# Run from this directory:\\n')
        f.write('\\n')
        f.write('# Windows Command Prompt:\\n')
        f.write(f'python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash {flash_opts} 0x20000 p3a.bin\\n')
        f.write('\\n')
        f.write('# Windows PowerShell:\\n')
        f.write(f'python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash {flash_opts} 0x20000 p3a.bin\\n')
        f.write('\\n')
        f.write('# Linux/macOS:\\n')
        f.write(f'python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default_reset --after hard_reset write_flash {flash_opts} 0x20000 p3a.bin\\n')
        f.write('\\n')
        f.write('\\n')
        f.write('# Or with explicit port (replace COM5 or /dev/ttyUSB0 with your port)\\n')
        f.write(f'python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts} 0x20000 p3a.bin\\n')

    print(f'  OK: flash_command_app_only.txt (dev build)')

# Generate README.md
readme_path = os.path.join(release_dir, 'README.md')
with open(readme_path, 'w') as f:
    f.write(f'''# p3a Firmware v{version}

This folder contains all files needed to flash the p3a firmware to an ESP32-P4 device
(e.g., Waveshare ESP32-P4-WiFi6-Touch-LCD board).

After the initial flash, all future updates are installed wirelessly via `http://p3a.local/ota`.

---

## Option 1: p3a Flasher (Windows) — Recommended

The easiest way to flash your p3a on Windows.

1. Connect your p3a via USB-C
2. Run `p3a-flasher.exe` (included in this folder)
3. Click **Flash Device**
4. Wait ~2 minutes
5. Done! Your device will automatically reboot into p3a.

The flasher auto-detects your device and includes the firmware — no installation, configuration or Internet connection needed.

---

## Option 2: Command Line (All Platforms)

For macOS, Linux, or if you prefer the command line on Windows.

### Prerequisites

1. **Python 3.x** with `esptool` installed:
   ```
   pip install esptool
   ```

2. **USB driver** for your ESP32-P4 board

3. Know your **serial port** (e.g., `COM5` on Windows, `/dev/ttyUSB0` on Linux)

### Quick Flash

Open a terminal in this folder and run:

```bash
# Windows PowerShell (replace COM5 with your port)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} \"@flash_args\"

# Windows Command Prompt (replace COM5 with your port)
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args

# Linux/macOS (replace /dev/ttyUSB0 with your port)
python -m esptool --chip esp32p4 -p /dev/ttyUSB0 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args
```

### Full Erase + Flash (if having issues)

If you're having issues or this is a fresh device, erase the flash first:

```bash
# Erase all flash (WARNING: erases all saved settings!)
python -m esptool --chip esp32p4 -p COM5 erase_flash

# Then flash
python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default-reset --after hard-reset write-flash {flash_opts}{force_flag} @flash_args
```

---

## After Flashing

The device will automatically reboot into p3a:
1. The p3a splash screen appears
2. Connect to the `p3a-setup` Wi-Fi network to configure your Wi-Fi
3. Access your p3a at `http://p3a.local/`

---

## Files Included

| File | Description | Flash Address |
|------|-------------|---------------|
| `p3a-flasher.exe` | Windows flasher (recommended) | — |
| `p3a.bin` | Main application firmware | 0x20000 |
| `bootloader.bin` | ESP-IDF bootloader | 0x2000 |
| `partition-table.bin` | Partition table | 0x8000 |
| `ota_data_initial.bin` | OTA data (boot selection) | 0x10000 |
| `storage.bin` | LittleFS web UI assets (4 MB) | 0x1020000 |
| `network_adapter.bin` | ESP32-C6 co-processor firmware | 0x1420000 |

Each `.bin` file has a corresponding `.sha256` file containing its SHA256 checksum.

Note: `--force` is needed for command-line flashing because `network_adapter.bin` is for ESP32-C6, not ESP32-P4.
It's stored in a data partition and transferred to the co-processor via OTA at boot.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| \"No serial port found\" | Try a different USB cable - many are charge-only |
| \"Failed to connect\" | Hold the **BOOT** button while connecting |
| Permission denied (Linux) | Run: `sudo usermod -a -G dialout $USER` then log out/in |
| Wrong COM port (Windows) | Check Device Manager > Ports |
| Timeout errors | Try `-b 115200` instead of `-b 460800` |

---

## Checksum Verification

To verify file integrity:

```bash
# Windows PowerShell
Get-FileHash p3a.bin -Algorithm SHA256 | Select-Object Hash

# Linux/macOS
sha256sum p3a.bin
```

Compare with contents of `p3a.bin.sha256`.
''')

print(f'  OK: README.md')

# Generate manifest.json for OTA updates
# This file contains version info and SHA256 hashes for both firmware and web UI
import json

manifest = {
    'api_version': api_version,
    'firmware': {
        'version': version,
        'file': 'p3a.bin',
        'sha256': None  # Will be filled below
    },
    'webui': {
        'version': webui_version,
        'requires_api_version': api_version,
        'file': 'storage.bin',
        'sha256': None  # Will be filled below
    }
}

# Read SHA256 from the .sha256 files we already generated
p3a_sha_path = os.path.join(release_dir, 'p3a.bin.sha256')
storage_sha_path = os.path.join(release_dir, 'storage.bin.sha256')

if os.path.exists(p3a_sha_path):
    with open(p3a_sha_path, 'r') as f:
        manifest['firmware']['sha256'] = f.read().strip()

if os.path.exists(storage_sha_path):
    with open(storage_sha_path, 'r') as f:
        manifest['webui']['sha256'] = f.read().strip()

manifest_path = os.path.join(release_dir, 'manifest.json')
with open(manifest_path, 'w') as f:
    json.dump(manifest, f, indent=2)

print(f'  OK: manifest.json')
print(f'      Firmware: {version} (SHA256: {manifest[\"firmware\"][\"sha256\"][:16] if manifest[\"firmware\"][\"sha256\"] else \"N/A\"}...)')
print(f'      Web UI:   {webui_version} (SHA256: {manifest[\"webui\"][\"sha256\"][:16] if manifest[\"webui\"][\"sha256\"] else \"N/A\"}...)')

print(f'\\n========== Release files ready ==========\\n')
")

# Add custom target that can be invoked manually: idf.py release
add_custom_target(release
    COMMAND ${PYTHON} "${CMAKE_BINARY_DIR}/create_release.py" "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Creating release package..."
    VERBATIM
)

# Make release depend on the main app being built
add_dependencies(release gen_project_binary)

# Hook into the build to run release automatically after p3a.bin is created
# gen_project_binary is the ESP-IDF target that creates the .bin file
add_custom_command(
    TARGET gen_project_binary POST_BUILD
    COMMAND ${PYTHON} "${CMAKE_BINARY_DIR}/create_release.py" "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Creating release package..."
    VERBATIM
)


if(P3A_BUILD_FLASHER AND CMAKE_HOST_WIN32)
    message(STATUS "Flasher build: ENABLED (set -DP3A_BUILD_FLASHER=OFF to disable)")
    
    # Build flasher after release files are created
    add_custom_command(
        TARGET gen_project_binary POST_BUILD
        COMMAND ${PYTHON} "${CMAKE_SOURCE_DIR}/flasher/build_flasher.py" 
                "${CMAKE_SOURCE_DIR}" 
                "${PROJECT_VER}" 
                "${CMAKE_SOURCE_DIR}/${RELEASE_SUBDIR}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building p3a-flasher.exe with embedded firmware..."
        VERBATIM
    )
else()
    if(NOT CMAKE_HOST_WIN32)
        message(STATUS "Flasher build: SKIPPED (Windows only)")
    else()
        message(STATUS "Flasher build: DISABLED (set -DP3A_BUILD_FLASHER=ON to enable)")
    endif()
endif()

