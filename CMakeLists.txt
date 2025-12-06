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
set(PROJECT_VER "0.5.0-dev")
# =============================================================================

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifiers)
project(p3a)

# Create SPIFFS image from webui directory
# When PICO-8 is disabled, filter out PICO-8 related assets to reduce binary size
if(CONFIG_P3A_PICO8_ENABLE)
    # PICO-8 enabled: use full webui directory
    spiffs_create_partition_image(storage webui FLASH_IN_PROJECT)
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
    
    spiffs_create_partition_image(storage "${WEBUI_STAGING_DIR}" FLASH_IN_PROJECT)
endif()

# =============================================================================
# Post-Build: Copy binaries to release directory and generate SHA256
# =============================================================================
# This creates a helper script that runs after the build to:
# 1. Copy binaries to build/release/v{version}/ (or /dev/ for dev builds)
# 2. Generate .sha256 checksum files

# Determine if this is a dev build (contains -dev, -alpha, -beta, -rc, -test)
string(REGEX MATCH "-(dev|alpha|beta|rc|test)" IS_DEV_BUILD "${PROJECT_VER}")

if(IS_DEV_BUILD)
    set(RELEASE_SUBDIR "release/v${PROJECT_VER}/dev")
else()
    set(RELEASE_SUBDIR "release/v${PROJECT_VER}")
endif()

# Create release helper script at configure time
file(WRITE "${CMAKE_BINARY_DIR}/create_release.py" "
#!/usr/bin/env python3
import os
import sys
import hashlib
import shutil

build_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
release_subdir = '${RELEASE_SUBDIR}'
release_dir = os.path.join(build_dir, release_subdir)

# Create release directory
os.makedirs(release_dir, exist_ok=True)

# Files to copy (source relative to build_dir, destination name, flash address)
files = [
    ('p3a.bin', 'p3a.bin', '0x20000'),
    ('bootloader/bootloader.bin', 'bootloader.bin', '0x2000'),
    ('partition_table/partition-table.bin', 'partition-table.bin', '0x8000'),
    ('ota_data_initial.bin', 'ota_data_initial.bin', '0x10000'),
    ('storage.bin', 'storage.bin', '0x1020000'),
]

print(f'\\n========== Creating Release: ${PROJECT_VER} ==========')
print(f'Output directory: {release_dir}\\n')

flash_entries = []

for src_rel, dst_name, flash_addr in files:
    src = os.path.join(build_dir, src_rel)
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

# Generate flash_args file
flash_args_path = os.path.join(release_dir, 'flash_args')
with open(flash_args_path, 'w') as f:
    f.write('--flash_mode dio --flash_freq 80m --flash_size 32MB\\n')
    for addr, name in flash_entries:
        f.write(f'{addr} {name}\\n')

print(f'  OK: flash_args')

# Generate flash command helper
flash_cmd_path = os.path.join(release_dir, 'flash_command.txt')
with open(flash_cmd_path, 'w') as f:
    f.write('# Flash command for ESP32-P4\\n')
    f.write('# Run from this directory:\\n')
    f.write('python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash @flash_args\\n')
    f.write('\\n')
    f.write('# Or with explicit port:\\n')
    f.write('python -m esptool --chip esp32p4 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash @flash_args\\n')

print(f'  OK: flash_command.txt')

print(f'\\n========== Release files ready ==========\\n')
")

# Add custom target that can be invoked manually: idf.py release
add_custom_target(release
    COMMAND ${PYTHON} "${CMAKE_BINARY_DIR}/create_release.py" "${CMAKE_BINARY_DIR}"
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
    COMMAND ${PYTHON} "${CMAKE_BINARY_DIR}/create_release.py" "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Creating release package..."
    VERBATIM
)

