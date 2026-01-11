# p3a Flasher Build Script (Windows)
# Creates a standalone executable using PyInstaller
#
# Usage: .\build.ps1
#
# Requirements:
#   - Python 3.8+ with pip
#   - Run: pip install -r requirements.txt

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  p3a Flasher Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check Python
Write-Host "Checking Python..." -ForegroundColor Yellow
try {
    $pythonVersion = python --version
    Write-Host "  Found: $pythonVersion" -ForegroundColor Green
} catch {
    Write-Host "  ERROR: Python not found. Please install Python 3.8+" -ForegroundColor Red
    exit 1
}

# Install dependencies
Write-Host ""
Write-Host "Installing dependencies..." -ForegroundColor Yellow
pip install -r requirements.txt --quiet
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to install dependencies" -ForegroundColor Red
    exit 1
}
Write-Host "  Done" -ForegroundColor Green

# Clean previous build
Write-Host ""
Write-Host "Cleaning previous build..." -ForegroundColor Yellow
if (Test-Path "dist") { Remove-Item -Recurse -Force "dist" }
if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
if (Test-Path "*.spec") { Remove-Item -Force "*.spec" }
Write-Host "  Done" -ForegroundColor Green

# Build executable
Write-Host ""
Write-Host "Building executable..." -ForegroundColor Yellow
Write-Host "  This may take a few minutes..." -ForegroundColor Gray

pyinstaller `
    --name "p3a-flasher" `
    --onefile `
    --windowed `
    --icon "p3a_icon.ico" `
    --add-data "p3a_logo.png;." `
    --add-data "p3a_icon.ico;." `
    --collect-data "esptool" `
    --hidden-import "serial.tools.list_ports" `
    --hidden-import "serial.tools.list_ports_windows" `
    --hidden-import "PIL._tkinter_finder" `
    p3a_flasher.py

if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Build failed" -ForegroundColor Red
    exit 1
}

Write-Host "  Done" -ForegroundColor Green

# Check output
$exePath = "dist\p3a-flasher.exe"
if (Test-Path $exePath) {
    $size = (Get-Item $exePath).Length / 1MB
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  Build successful!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Output: $exePath" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($size, 1)) MB" -ForegroundColor White
    Write-Host ""
    Write-Host "  To run: .\dist\p3a-flasher.exe" -ForegroundColor Cyan
} else {
    Write-Host ""
    Write-Host "  ERROR: Executable not found" -ForegroundColor Red
    exit 1
}

