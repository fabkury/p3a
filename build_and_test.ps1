# Build and test script for ESP32-P4 firmware
# This script builds the firmware and optionally flashes it

param(
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "COM5"
)

Write-Host "Building firmware..." -ForegroundColor Cyan

# Try to find ESP-IDF
$idfPath = $env:IDF_PATH
if (-not $idfPath) {
    Write-Host "ERROR: IDF_PATH not set. Please activate ESP-IDF environment first." -ForegroundColor Red
    Write-Host "Run: . $env:USERPROFILE\.espressif\esp-idf\export.ps1" -ForegroundColor Yellow
    exit 1
}

# Build
& idf.py build
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Build successful!" -ForegroundColor Green

if ($Flash) {
    Write-Host "Flashing firmware..." -ForegroundColor Cyan
    & idf.py -p $Port flash
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Flash failed!" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "Flash successful!" -ForegroundColor Green
}

if ($Monitor) {
    Write-Host "Starting monitor..." -ForegroundColor Cyan
    & python ..\tools\serial_monitor.py --port $Port --duration 30
}

