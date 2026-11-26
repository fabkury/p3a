#!/usr/bin/env python3
"""
p3a Flasher Build Script

Builds p3a-flasher.exe with a release's files embedded. Run by hand, after
tools/make_release.py has made the release folder; the flasher is not part
of the firmware build.

Usage:
    python host/p3a-flasher/build_flasher.py release/v<version>

What is embedded is what the folder's flash_args lists (flash_plan.py).
"""

import os
import sys
import shutil
import subprocess
import tempfile
import venv
from pathlib import Path


def find_system_python():
    """Find a system Python that has tkinter (not the ESP-IDF minimal Python)."""
    candidates = []

    # On Windows, try the py launcher first (most reliable)
    if sys.platform == 'win32':
        candidates.append(['py', '-3'])

    candidates.extend([['python3'], ['python']])

    for cmd in candidates:
        try:
            result = subprocess.run(
                cmd + ['-c', 'import tkinter; import sys; print(sys.executable)'],
                capture_output=True, text=True, timeout=10
            )
            if result.returncode == 0:
                python_path = result.stdout.strip()
                # Skip the ESP-IDF Python (it doesn't have tkinter anyway,
                # but guard against future changes)
                if 'espressif' not in python_path.lower():
                    return python_path
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue

    return None


def get_or_create_venv(flasher_dir):
    """Get or create a cached venv with flasher dependencies.

    The venv is created from a system Python (with tkinter) and cached
    at flasher/.venv so it persists across builds.
    """
    venv_dir = flasher_dir / '.venv'
    requirements_file = flasher_dir / 'requirements.txt'

    if sys.platform == 'win32':
        venv_python = venv_dir / 'Scripts' / 'python.exe'
    else:
        venv_python = venv_dir / 'bin' / 'python'

    # Check if existing venv is usable (has PyInstaller and tkinter)
    if venv_python.exists():
        check = subprocess.run(
            [str(venv_python), '-c', 'import tkinter; import PyInstaller'],
            capture_output=True, text=True
        )
        if check.returncode == 0:
            print(f"  Using cached venv: {venv_dir}")
            return str(venv_python)
        else:
            print(f"  Cached venv is stale, recreating...")
            shutil.rmtree(venv_dir, ignore_errors=True)

    # Find system Python with tkinter
    system_python = find_system_python()
    if not system_python:
        print("ERROR: No system Python with tkinter found.")
        print("       The ESP-IDF Python does not include tkinter.")
        print("       Install a standard Python from https://python.org")
        sys.exit(1)

    print(f"  System Python: {system_python}")
    print(f"  Creating venv: {venv_dir}")

    # Create venv using the system Python
    result = subprocess.run(
        [system_python, '-m', 'venv', str(venv_dir)],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"ERROR: Failed to create venv:\n{result.stderr}")
        sys.exit(1)

    # Install requirements
    print(f"  Installing flasher dependencies...")
    result = subprocess.run(
        [str(venv_python), '-m', 'pip', 'install', '-q', '-r', str(requirements_file)],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"ERROR: Failed to install dependencies:\n{result.stderr}")
        shutil.rmtree(venv_dir, ignore_errors=True)
        sys.exit(1)

    print(f"  Venv ready.")
    return str(venv_python)


def main():
    # Force UTF-8 output on Windows to support Unicode symbols
    if sys.platform == 'win32':
        import io
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

    if len(sys.argv) == 2:
        # The usual way: the release folder alone. tools/make_release.py names it v<version>.
        release_dir = Path(sys.argv[1]).resolve()
        project_dir = Path(__file__).resolve().parents[2]
        version = release_dir.name[1:] if release_dir.name.startswith('v') else release_dir.name
    elif len(sys.argv) == 4:
        project_dir = Path(sys.argv[1]).resolve()
        version = sys.argv[2]
        release_dir = Path(sys.argv[3]).resolve()
    else:
        print("Usage: python build_flasher.py <release_dir>")
        print("   or: python build_flasher.py <project_dir> <version> <release_dir>")
        print("Example: python host/p3a-flasher/build_flasher.py release/v2.0.0")
        sys.exit(1)

    flasher_dir = project_dir / 'host' / 'p3a-flasher'
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

    # Get or create a Python venv with tkinter and flasher deps
    venv_python = get_or_create_venv(flasher_dir)
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

    # What the release writes, as its own flash_args says (tools/make_release.py).
    sys.path.insert(0, str(flasher_dir))
    import flash_plan
    try:
        plan = flash_plan.load(str(release_dir))
    except flash_plan.PlanError as why:
        print(f"ERROR: {release_dir} is not a release folder: {why}")
        sys.exit(1)
    firmware_files = [flash_plan.ARGS_NAME] + [os.path.basename(path) for _, path in plan]

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
        shutil.copy2(flasher_dir / 'flash_plan.py', build_dir / 'flash_plan.py')

        # Build with PyInstaller
        print("\nRunning PyInstaller...")

        pyinstaller_args = [
            venv_python, '-m', 'PyInstaller',
            '--name', 'p3a-flasher',
            '--onefile',
            '--windowed',
            '--icon', 'p3a_icon.ico',
            '--add-data', f'p3a_logo.png{os.pathsep}.',
            '--add-data', f'p3a_icon.ico{os.pathsep}.',
            '--add-data', f'firmware{os.pathsep}firmware',
            '--hidden-import', 'flash_plan',
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

