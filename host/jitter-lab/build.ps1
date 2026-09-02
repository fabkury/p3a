# build.ps1 — build p3a in release or diag configuration (jitter work stream).
#
#   pwsh host/jitter-lab/build.ps1                # release: build/  (tracked sdkconfig, trace OFF)
#   pwsh host/jitter-lab/build.ps1 -Diag          # diag:    build-diag/ (sdkconfig + sdkconfig.diag.defaults)
#   pwsh host/jitter-lab/build.ps1 -Diag -Flash   # ...then flash the dev unit (COM5) without monitor
#   pwsh host/jitter-lab/build.ps1 -Diag -Extra sdkconfig.nowrap.defaults -Suffix nowrap
#                                                 # diag + extra overlay(s) into build-diag-nowrap/
#   pwsh host/jitter-lab/build.ps1 -Diag -Suffix patch -FlashOnly
#                                                 # flash an existing build dir with esptool, NO rebuild
#                                                 # (safe when the IDF tree no longer matches that build)
#
# Guards: the release sdkconfig must be byte-identical before and after any
# build (git diff), and the silicon-rev lines must survive. Both are checked.
# Every build/flash prints the ARM line: which sdmmc_wait_for_idle variant the
# binary carries (p3a's --wrap, and/or the esp-idf #19034 patch) + sha256.
param(
    [switch]$Diag,
    [switch]$Flash,
    [switch]$FlashOnly,
    [switch]$FullClean,
    [string]$Suffix = "",
    [string[]]$Extra = @(),
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
    if ($Suffix) { $buildDir = "build-diag-$Suffix" }
    $sources = @("sdkconfig", "sdkconfig.diag.defaults") + $Extra
    foreach ($src in $Extra) { if (-not (Test-Path $src)) { throw "overlay not found: $src" } }
    $args = @("-B", $buildDir, "-DSDKCONFIG=$buildDir/sdkconfig", "-DSDKCONFIG_DEFAULTS=$($sources -join ';')")
    # SDKCONFIG_DEFAULTS only apply when the sdkconfig is CREATED. If any
    # source is newer than the generated build dir sdkconfig, regenerate it.
    $gen = "$buildDir/sdkconfig"
    if ((Test-Path $gen) -and -not $FlashOnly) {
        $genTime = (Get-Item $gen).LastWriteTime
        foreach ($src in $sources) {
            if ((Get-Item $src).LastWriteTime -gt $genTime) {
                Write-Host "diag sdkconfig is older than $src -> regenerating"
                Remove-Item $gen -Force
                break
            }
        }
    }
} else {
    if ($Suffix -or $Extra.Count) { throw "-Suffix/-Extra are for -Diag builds only" }
    $buildDir = "build"
    $args = @()
}

function Show-Arm($dir) {
    $map = Join-Path $dir "p3a.map"
    $bin = Join-Path $dir "p3a.bin"
    if (-not (Test-Path $map) -or -not (Test-Path $bin)) { Write-Host "ARM: (no map/bin in $dir)"; return }
    # Defined functions only (.text.<name> sections): the bare symbol name also
    # appears in the map for the weak reference in sd_idle_wait_info.c.
    $wrap = (Select-String -Path $map -Pattern '^\s*\.text\.__wrap_sdmmc_wait_for_idle' -Quiet)
    $patch = (Select-String -Path $map -Pattern '^\s*\.text\.sdmmc_poll_delay_and_backoff' -Quiet)
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
    Write-Host ("ARM: dir={0} wrap={1} idf_patch={2} p3a.bin sha256={3} built={4}" -f $dir, $wrap, $patch, $sha, (Get-Item $bin).LastWriteTime.ToString("s"))
}

if (-not $FlashOnly) {
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
    $wrapCfg = ($cfg -match '^CONFIG_P3A_SD_IDLE_WAIT_WRAP=y').Count -gt 0
    if (-not $Diag -and -not $wrapCfg) { throw "GUARD: release build without CONFIG_P3A_SD_IDLE_WAIT_WRAP=y" }
    Write-Host ("BUILD OK: {0} (frame_trace={1}, sd_idle_wait_wrap={2})" -f $buildDir, $trace, $wrapCfg)
}
Show-Arm $buildDir

if ($Flash -or $FlashOnly) {
    if ($FlashOnly) {
        # esptool directly from the build dir's flash_args: no ninja, no rebuild.
        Push-Location $buildDir
        try {
            & python -m esptool --chip esp32p4 -p $Port -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
            if ($LASTEXITCODE -ne 0) { throw "esptool write-flash failed" }
        } finally { Pop-Location }
    } else {
        & idf.py @args -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw "idf.py flash failed" }
    }
    Write-Host "FLASH OK: $Port <- $buildDir"
}
