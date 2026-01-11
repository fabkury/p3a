#!/usr/bin/env python3
"""
p3a Flasher Build Script

Builds the p3a-flasher.exe with embedded firmware binaries.
Called automatically by CMake after the main build (if enabled).

Usage:
    python build_flasher.py <project_dir> <version> <release_dir>
    
Arguments:
    project_dir  - Path to p3a project root
    version      - Firmware version (e.g., "0.6.0-dev")
    release_dir  - Path to release directory containing firmware binaries
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path


def main():
    # Force UTF-8 output on Windows to support Unicode symbols
    if sys.platform == 'win32':
        import io
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

    if len(sys.argv) < 4:
        print("Usage: python build_flasher.py <project_dir> <version> <release_dir>")
        print("Example: python build_flasher.py . 0.6.0-dev release/v0.6.0-dev/dev")
        sys.exit(1)
    
    project_dir = Path(sys.argv[1]).resolve()
    version = sys.argv[2]
    release_dir = Path(sys.argv[3]).resolve()
    
    flasher_dir = project_dir / 'flasher'
    flasher_py = flasher_dir / 'p3a_flasher.py'
    logo_file = flasher_dir / 'p3a_logo.png'
    icon_file = flasher_dir / 'p3a_icon.ico'
    
    print(f"\n{'='*60}")
    print(f"  Building p3a Flasher v{version}")
    print(f"{'='*60}")
    print(f"  Project:  {project_dir}")
    print(f"  Release:  {release_dir}")
    print(f"  Flasher:  {flasher_dir}")
    print()
    
    # Verify source files exist
    if not flasher_py.exists():
        print(f"ERROR: Flasher script not found: {flasher_py}")
        sys.exit(1)
    
    if not logo_file.exists():
        print(f"ERROR: Logo not found: {logo_file}")
        sys.exit(1)

    if not icon_file.exists():
        print(f"ERROR: Icon not found: {icon_file}")
        sys.exit(1)
        
    if not release_dir.exists():
        print(f"ERROR: Release directory not found: {release_dir}")
        sys.exit(1)
    
    # Required firmware files
    firmware_files = [
        'bootloader.bin',
        'partition-table.bin',
        'ota_data_initial.bin',
        'p3a.bin',
        'storage.bin',
        'network_adapter.bin',
    ]
    
    # Verify all firmware files exist
    missing = []
    for f in firmware_files:
        if not (release_dir / f).exists():
            missing.append(f)
    
    if missing:
        print(f"ERROR: Missing firmware files in {release_dir}:")
        for f in missing:
            print(f"  - {f}")
        sys.exit(1)
    
    # Create temporary build directory
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        build_dir = temp_path / 'build'
        build_dir.mkdir()
        
        # Create firmware subdirectory
        firmware_dir = build_dir / 'firmware'
        firmware_dir.mkdir()
        
        # Copy firmware files
        print("Copying firmware files...")
        for f in firmware_files:
            src = release_dir / f
            dst = firmware_dir / f
            shutil.copy2(src, dst)
            size_kb = dst.stat().st_size / 1024
            print(f"  ✓ {f} ({size_kb:.1f} KB)")
        
        # Copy logo and icon
        shutil.copy2(logo_file, build_dir / 'p3a_logo.png')
        print(f"  ✓ p3a_logo.png")
        shutil.copy2(icon_file, build_dir / 'p3a_icon.ico')
        print(f"  ✓ p3a_icon.ico")
        
        # Create modified flasher script with embedded version
        print(f"\nSetting embedded version to: {version}")
        with open(flasher_py, 'r', encoding='utf-8') as f:
            flasher_code = f.read()
        
        # Replace the version placeholder
        flasher_code = flasher_code.replace(
            'EMBEDDED_VERSION = None',
            f'EMBEDDED_VERSION = "{version}"'
        )
        
        modified_flasher = build_dir / 'p3a_flasher.py'
        with open(modified_flasher, 'w', encoding='utf-8') as f:
            f.write(flasher_code)
        
        # Build with PyInstaller
        print("\nRunning PyInstaller...")
        
        pyinstaller_args = [
            'python', '-m', 'PyInstaller',
            '--name', 'p3a-flasher',
            '--onefile',
            '--windowed',
            '--icon', 'p3a_icon.ico',
            '--add-data', f'p3a_logo.png{os.pathsep}.',
            '--add-data', f'p3a_icon.ico{os.pathsep}.',
            '--add-data', f'firmware{os.pathsep}firmware',
            '--collect-data', 'esptool',
            '--hidden-import', 'serial.tools.list_ports',
            '--hidden-import', 'serial.tools.list_ports_windows',
            '--hidden-import', 'PIL._tkinter_finder',
            '--distpath', str(build_dir / 'dist'),
            '--workpath', str(build_dir / 'work'),
            '--specpath', str(build_dir),
            str(modified_flasher),
        ]
        
        result = subprocess.run(
            pyinstaller_args,
            cwd=build_dir,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print("PyInstaller FAILED:")
            print(result.stdout)
            print(result.stderr)
            sys.exit(1)
        
        # Copy the built executable to the release directory
        built_exe = build_dir / 'dist' / 'p3a-flasher.exe'
        if not built_exe.exists():
            print(f"ERROR: Built executable not found: {built_exe}")
            sys.exit(1)
        
        final_exe = release_dir / 'p3a-flasher.exe'
        shutil.copy2(built_exe, final_exe)
        
        size_mb = final_exe.stat().st_size / (1024 * 1024)
        print(f"\n{'='*60}")
        print(f"  ✓ Build successful!")
        print(f"{'='*60}")
        print(f"  Output: {final_exe}")
        print(f"  Size:   {size_mb:.1f} MB")
        print(f"  Version: {version}")
        print()


if __name__ == '__main__':
    main()

