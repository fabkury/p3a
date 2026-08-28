# build.ps1 — build p3a in release or diag configuration (jitter work stream).
#
#   pwsh host/jitter-lab/build.ps1                # release: build/  (tracked sdkconfig, trace OFF)
#   pwsh host/jitter-lab/build.ps1 -Diag          # diag:    build-diag/ (sdkconfig + sdkconfig.diag.defaults)
#   pwsh host/jitter-lab/build.ps1 -Diag -Flash   # ...then flash the dev unit (COM5) without monitor
#   pwsh host/jitter-lab/build.ps1 -Diag -Flash -Port COM5
#
# Guards: the release sdkconfig must be byte-identical before and after any
# build (git diff), and the silicon-rev lines must survive. Both are checked.
param(
    [switch]$Diag,
    [switch]$Flash,
    [switch]$FullClean,
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $repo

$env:PYTHONUTF8 = "1"
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1 | Out-Null
$env:ESP_IDF_VERSION = "5.5"   # see root CLAUDE.md (esp_wifi_remote Kconfig fragment)

$before = (git hash-object sdkconfig)

if ($Diag) {
    $buildDir = "build-diag"
    $args = @("-B", $buildDir, "-DSDKCONFIG=$buildDir/sdkconfig", "-DSDKCONFIG_DEFAULTS=sdkconfig;sdkconfig.diag.defaults")
    # SDKCONFIG_DEFAULTS only apply when the sdkconfig is CREATED. If either
    # source is newer than the generated build-diag/sdkconfig, regenerate it.
    $gen = "$buildDir/sdkconfig"
    if (Test-Path $gen) {
        $genTime = (Get-Item $gen).LastWriteTime
        foreach ($src in @("sdkconfig", "sdkconfig.diag.defaults")) {
            if ((Get-Item $src).LastWriteTime -gt $genTime) {
                Write-Host "diag sdkconfig is older than $src -> regenerating"
                Remove-Item $gen -Force
                break
            }
        }
    }
} else {
    $buildDir = "build"
    $args = @()
}

if ($FullClean) {
    & idf.py @args fullclean
}
& idf.py @args build
if ($LASTEXITCODE -ne 0) { throw "idf.py build failed ($buildDir)" }

# --- guards -------------------------------------------------------------
$after = (git hash-object sdkconfig)
if ($before -ne $after) { throw "GUARD: release sdkconfig changed during the build. Inspect 'git diff sdkconfig' and restore." }

$cfgPath = if ($Diag) { "$buildDir/sdkconfig" } else { "sdkconfig" }   # release builds use the tracked file in place
$cfg = Get-Content $cfgPath
if (-not ($cfg -match '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y') -or -not ($cfg -match '^CONFIG_ESP32P4_REV_MIN_1=y')) {
    throw "GUARD: $cfgPath lost the ESP32-P4 rev v1.0 guards (see CLAUDE.md)."
}
$trace = ($cfg -match '^CONFIG_P3A_FRAME_TRACE=y').Count -gt 0
if ($Diag -and -not $trace) { throw "GUARD: diag build without CONFIG_P3A_FRAME_TRACE=y" }
if (-not $Diag -and $trace) { throw "GUARD: release build has CONFIG_P3A_FRAME_TRACE=y" }
Write-Host ("BUILD OK: {0} (frame_trace={1})" -f $buildDir, $trace)

if ($Flash) {
    & idf.py @args -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw "idf.py flash failed" }
    Write-Host "FLASH OK: $Port"
}
