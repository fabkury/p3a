# ESP-IDF v5.5.4 — Workstation 2 Upgrade Crib Sheet

Workstation 1 was upgraded 2026-06-07 (see commits `780d153f`…`076e91cc`). All
project-side changes (sdkconfig, `slave_ota` esptool `--force` fix, CLAUDE.md)
arrive via git — this machine only needs the toolchain installed and two
activation habits. Expected time: ~15 minutes, mostly download.

The existing v5.5.2 install (`C:\Espressif\Initialize-Idf.ps1 -IdfId
esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562`) stays untouched as a fallback.

## 1. Install ESP-IDF v5.5.4 via EIM

```powershell
# Download the EIM CLI (~21 MB)
New-Item -ItemType Directory -Force C:\Users\Fab\esp\eim | Out-Null
Invoke-WebRequest -Uri "https://github.com/espressif/idf-im-ui/releases/download/v0.13.1/eim-cli-windows-x64.exe" -OutFile C:\Users\Fab\esp\eim\eim.exe

# Run the install OUTSIDE the repo (it drops eim_config.toml into the cwd)
Set-Location C:\Users\Fab\esp\eim
.\eim.exe install -i v5.5.4 -t esp32p4,esp32c6 -n true -a true --cleanup true --do-not-track true
```

Takes ~10 min (downloads IDF + toolchains). Result:
- ESP-IDF → `C:\esp\v5.5.4\esp-idf` (note: NOT `C:\Espressif\frameworks\...` — EIM 0.13.x changed the default)
- Tools/Python → `C:\Espressif\tools` (shared root with the v5.5.2 install; they coexist)
- Activation script → `C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1`

## 2. Activation (the part that bites)

```powershell
$env:PYTHONUTF8="1"
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
$env:ESP_IDF_VERSION="5.5"   # REQUIRED — see landmine #1
```

### Landmine #1: EIM sets the wrong `ESP_IDF_VERSION` format

EIM 0.13.1's profile script sets `ESP_IDF_VERSION` to the full version
(`"5.5.4"`), but official IDF 5.5.x activation (`export.ps1`/`activate.py`)
sets major.minor (`"5.5"`). `esp_wifi_remote`'s Kconfig
(`orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"`) keys its fragment files on
the official convention — `Kconfig.idf_v5.5.in` exists, `Kconfig.idf_v5.5.4.in`
does not (in our pinned 1.2.2). With the wrong value the fragment silently
loads nothing, every `SLAVE_IDF_TARGET_*` symbol vanishes, sdkconfig
regenerates with esp_hosted on SPI/`"invalid"` slave target, and the build dies
with a misleading `#error "Unknown Slave Target"` deep inside esp_hosted.
The `$env:ESP_IDF_VERSION="5.5"` override AFTER sourcing the profile fixes it.

**Recovery if it happens:** `git checkout -- sdkconfig dependencies.lock`,
delete `build/`, set the env var, rebuild.

## 3. Build and verify

```powershell
git pull   # brings sdkconfig, slave_ota fix, CLAUDE.md
# If a build/ dir exists from v5.5.2, delete it first (retry if Dropbox holds
# files — also check for orphaned idf.py/monitor processes from old sessions)
idf.py build
```

**The gate: after a successful build, `git diff sdkconfig` must be EMPTY.**
- Any diff touching `ESP32P4_REV_MIN_FULL`, `ESP_HOSTED_*`, or
  `SLAVE_IDF_TARGET_*` → landmine #1 fired; recover as above.
- Workstation 1's committed sdkconfig was regenerated under v5.5.4 with
  `ESP_IDF_VERSION` set, so a correct build reproduces it byte-for-byte.

## 4. Flash and boot check

```powershell
idf.py -p COM<N> flash monitor   # COM port may differ from workstation 1 (COM11)
```

- `idf.py flash` works under esptool v5 thanks to the `--force` in
  `slave_ota/CMakeLists.txt` (esptool v5 otherwise refuses the embedded
  ESP32-C6 `network_adapter.bin` — chip id 13 in a P4 flash image set).
- **If this machine flashes a different board** than workstation 1's: check the
  boot log for `chip revision:`. The committed sdkconfig supports **rev < 3.0
  only** (`ESP32P4_SELECTS_REV_LESS_V3=y`). A rev 3.x board will refuse this
  image and needs a separate build — rev <3.0 and ≥3.0 are mutually exclusive
  targets from IDF 5.5.2 onward.

Healthy boot milestones (same as workstation 1's verification):

```
I boot: ESP-IDF v5.5.4 2nd stage bootloader
I boot: chip revision: v1.0
I esp_psram: SPI SRAM memory test OK          (PSRAM now runs 1.8V — upstream removed 1.9V)
I cpu_start: cpu freq: 360000000 Hz           (360 MHz is correct for rev <3 silicon)
I H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
I esp_netif_handlers: sta ip: ...
```

Known-benign log lines: `E system_api: 0 mac type is incorrect` (long-standing
P4 quirk) and the `slave_ota` warning about C6 esp_hosted 2.7.0
(esp-hosted-mcu#143, pre-existing).

## 5. Cleanup notes

- `eim_config.toml` appears wherever EIM ran — delete or keep outside the repo.
- Once this machine builds + flashes clean, the v5.5.2 fallback lines in
  CLAUDE.md can be retired.
- Component pins are deliberate and unchanged: `esp_hosted ~2.9.3` (2.10+
  blows the internal-RAM budget — see comment in `main/idf_component.yml`),
  `esp_wifi_remote 1.2.2` via lock (1.4.2+ requires esp_hosted ≥2.11, so do
  NOT `idf.py update-dependencies`).
