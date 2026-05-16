# ESP-IDF v6.0 Migration Report for p3a

**Generated:** 2026-05-16
**Revised:** 2026-05-16 — §2.6 corrected after upstream `esp_hosted` status check
**Source IDF version:** v5.5.1 (this workstation) / v5.5.2 (sibling workstation)
**Target IDF version:** v6.0.1 (current stable, released 2026-04-10)

## TL;DR

ESP-IDF v6.0 shipped on **2026-03-16**; **v6.0.1 (2026-04-10)** is the current bugfix release. p3a's source code is in unexpectedly good shape for the migration — every explicitly-removed symbol Espressif lists (deprecated Wi-Fi macros, removed OTA / NETIF / FreeRTOS / TLS APIs, old LCD config fields, dropped headers) was grep'd against the codebase and **found zero direct hits**. All the fragile peripheral plumbing lives behind the Waveshare BSP and Espressif's managed components.

The real risk surface is therefore **not in p3a's own code** — it's in **three external dependencies**: (1) the Waveshare BSP and ST7703 panel driver, (2) the mbedTLS v4 / PSA Crypto transition affecting every TLS endpoint p3a talks to, and (3) the toolchain bump to GCC 15 with warnings-as-errors on by default. Add to that the ESP32-P4 silicon-revision default change in v6.0. (`esp_hosted` is **not** on this list — its 2.9.x pin is compatible with v6.0 and stays in place across the migration; see §2.6.)

Recommendation: **stay on v5.5.x for now**, install v6.0.1 in parallel for experimentation, and revisit a serious migration after v6.0.2 (2026-06-04) and once Waveshare ships a v6.0-compatible BSP. Details below.

---

## 1. Installing ESP-IDF v6.0 alongside v5.5.1

The current setup already runs two installs side-by-side (the `C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1` path and the `C:\Espressif\Initialize-Idf.ps1 -IdfId ...` v5.5.2 path), so the mechanism is familiar. v6.0 adds one new wrinkle: **Espressif retired the legacy Windows Installer** with this release and replaced it with the **ESP-IDF Installation Manager (EIM)**.

**Recommended (EIM)**:
1. Download EIM (GUI or CLI) from <https://dl.espressif.com/dl/eim/> — or `winget install Espressif.EIM` if that package id resolves.
2. From EIM, install **v6.0.1** to `C:\Espressif\` alongside the existing v5.5.2 install. EIM handles multi-version cleanly and gives each install a distinct `IdfId`, so the existing `Initialize-Idf.ps1 -IdfId <new-id>` workflow continues to work.
3. v5.5.1 at `C:\Users\Fab\esp\v5.5.1\` is untouched — its `export.ps1` is independent.

**Manual alternative** (closer to the v5.5.1 layout):
```powershell
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git C:\Users\Fab\esp\v6.0.1\esp-idf
C:\Users\Fab\esp\v6.0.1\esp-idf\install.ps1
# Then per session:
C:\Users\Fab\esp\v6.0.1\esp-idf\export.ps1
```
This works and keeps the three versions cleanly separated, but misses EIM's update tooling.

**Toolchain prerequisites (v6.0)**: Python ≥ 3.10, CMake ≥ 3.22.1, GCC 15.1.0 (auto-installed by EIM or `install.ps1`). The VS Code ESP-IDF extension (≥ v1.9) auto-discovers EIM installs.

**Caution**: Don't try to share `C:\Espressif\tools\` between v5.5.x and v6.0 — they pull different toolchain versions. EIM (and `install.ps1`) handle this correctly; don't override `IDF_TOOLS_PATH`.

---

## 2. Required p3a Codebase Changes

Each item is graded **REQUIRED** (build breaks otherwise), **LIKELY** (build may succeed but runtime risk), or **OPTIONAL** (cleanup).

### 2.1 ESP32-P4 silicon revision — REQUIRED, ~1 line

Current `sdkconfig` has:
```
sdkconfig:1300:# CONFIG_ESP32P4_REV_MIN_0 is not set
sdkconfig:1301:CONFIG_ESP32P4_REV_MIN_1=y
sdkconfig:1309:CONFIG_ESP32P4_REV_MAX_FULL=199
```
v6.0 bumps the default minimum to v3.0 silicon. The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (purchased 2024-ish) almost certainly has **pre-v3 silicon**, so you'll need `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` (or set `CONFIG_ESP32P4_REV_MIN_1` explicitly and confirm `REV_MAX` permits it). One line in `menuconfig`. **Check the actual silicon rev with `esptool.py chip_id` or the `idf.py monitor` boot banner before guessing.**

### 2.2 Toolchain / GCC 15 / warnings-as-errors — LIKELY, hours of cleanup

v6.0 enables **warnings-as-errors by default** and adds new GCC 15 warnings (`-Wunterminated-string-initialization`, `-Wheader-guard`, several C++ ones). It also makes **orphan linker sections a hard error**. The default C standard moves to `gnu23`, C++ to `gnu++26`.

p3a is pure C11; no C++ standard library use, so the C++ standard bump is irrelevant. But the warnings pass on 26 custom components will produce noise. Two paths:
- **Quick escape hatch**: `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y` and `CONFIG_COMPILER_DISABLE_GCC15_WARNINGS=y`. Build immediately, clean up later.
- **Right way**: Fix warnings one component at a time. Expect ~2–8 hours depending on style discipline.

Also: `#include <sys/dirent.h>` no longer provides `opendir()` (use `<dirent.h>`); `<sys/signal.h>` removed under Picolibc. Grep found **none** of these in p3a, but transitive component code might trigger it.

### 2.3 Picolibc default — LIKELY benign, but verify

v6.0 defaults to Picolibc (was Newlib). Claimed wins: ~20% binary size reduction, much smaller stdio stacks. **Gotcha**: `stdin`/`stdout`/`stderr` are global, not per-task. p3a doesn't appear to redirect per-task stdio, so it should be fine. If anything misbehaves, the escape hatch is `CONFIG_LIBC_NEWLIB=y`.

### 2.4 esp-mqtt component move — REQUIRED, 1 dependency add

`mqtt_client.h` is used at `components/makapix/makapix_mqtt.c:12`. In v6.0, `esp-mqtt` moves out of the core tree to the registry:
```bash
idf.py add-dependency "espressif/mqtt"
```
The header path and API are **unchanged** — only the dependency declaration changes.

### 2.5 mbedTLS v4 + PSA Crypto — REQUIRED, ~10 lines + runtime audit

This is the most invasive non-vendor change.

- **Add `psa_crypto_init()`** somewhere in the boot path before any TLS work happens (Makapix MQTT, Giphy HTTPS, museum IIIF endpoints, GitHub Releases OTA). Logical home: `main/p3a_main.c` just after `nvs_flash_init()`. Returns `PSA_SUCCESS` (0) on success.
- **Verify TLS endpoints still trust**:
  - `*.giphy.com`
  - Makapix MQTT broker (whatever CA chain it uses)
  - `iiif.artic.edu`, `iiif.wellcomecollection.org`, `iiif.smk.dk`, `data.rijksmuseum.nl`, `framemark.vam.ac.uk` (the museum endpoints)
  - `api.github.com` / `objects.githubusercontent.com` (OTA)

  v6.0 removed deprecated CAs from the bundled cert bundle and dropped cipher suites without forward secrecy (static RSA, static ECDH, FFDHE) and EC curves below 250 bits. Modern endpoints are fine; legacy ones can break silently. Test each endpoint after migration.
- **+37 KB flash growth in `esp_http_client`** because of PSA migration. p3a's partitions are 8 MB OTA slots, so this is comfortably absorbed.
- No direct mbedTLS primitive calls in p3a code (no `esp_aes_*`, no `esp_ecdsa_*` hits) — only the implicit TLS-via-`esp_http_client`/`mqtt` path matters.

### 2.6 ESP-Hosted / esp_wifi_remote — NO `esp_hosted` bump required for v6.0

Currently in `dependencies.lock`:
```
esp_hosted: 2.9.7  (held by manifest constraint "~2.9.3" in main/idf_component.yml)
esp_wifi_remote: 1.2.2
```

Both 2.9.x and the current **2.12.7** declare `idf: ">=5.3"` with no upper bound, so the 2.9.7 pin is **compatible with v6.0 as-is** — no `esp_hosted` bump is required to migrate. The 2.12.7 changelog explicitly notes a v6.x PicolibC build fix, confirming v6.x is an actively-supported target on the latest line.

For `esp_wifi_remote`: **1.5.2** (current) ships dedicated `Kconfig.idf_v6.0.in` and `Kconfig.idf_v6.1.in`. Bump from 1.2.2 → 1.5.2 is straightforward; the public API is unchanged across these semver-minor steps.

**Why we're staying pinned at `esp_hosted` 2.9.x** rather than upgrading along with the migration: 2.12.6+ intentionally preallocates ~47.6 KB of DMA-capable internal SRAM at init (31 mempool buckets × 1536 bytes) instead of the prior lazy-allocation scheme. On the ESP32-P4's ~70 KB DMA-internal pool, this exceeds p3a's current sdkconfig budget and produces `HS_MP "no mem"` at boot followed by an SDIO host self-reset. The change is **acknowledged as intentional and won't be reverted** (Espressif on [esp-hosted-mcu#191](https://github.com/espressif/esp-hosted-mcu/issues/191), 2026-05-04: *"reverting to it is not a viable path forward… may need to reduce the number of mempool buckets or consider offloading some of them to PSRAM"*). No Kconfig knob to bound the pool exists yet; mitigation is on Espressif's TODO but unscheduled. The clean path is therefore to **keep `esp_hosted` at 2.9.x across the v6.0 migration**, and treat any future bump as a separate workstream paired with both a sdkconfig DMA-internal-SRAM increase and the switch to SDIO packet mode tracked in [docs/sdio-rx-oom-crash.md](../sdio-rx-oom-crash.md).

No direct `esp_wifi_*` macro hits for any of v6.0's renamed Wi-Fi symbols in p3a code — the `wifi_manager` is conservative.

### 2.7 LCD / MIPI-DSI — LIKELY transparent, depends on vendor BSP

v6.0 breaks the `esp_lcd` panel-config struct shape (`psram_trans_align` → `dma_burst_size`, `color_space`/`rgb_endian` → `rgb_ele_order`, `gpio_num_t` typing, `esp_lcd_panel_disp_off()` → `esp_lcd_panel_disp_on_off()`, etc.). Greps found **zero direct uses** of any of these fields in p3a — the Waveshare `esp32_p4_wifi6_touch_lcd_4b` BSP and `esp_lcd_st7703` driver absorb all of it.

**The risk shifts to Waveshare**: their BSP is currently at v1.0.1, ST7703 at v1.0.5. Vendor BSPs typically lag IDF major releases by 1–3 months. **Before starting migration**, check <https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_4b> for a v6.0-compatible release. If none exists, two options:
- Wait for Waveshare to update.
- Fork the BSP locally, patch the panel-config struct initializers (~20–50 lines), and use `path:` overrides in `idf_component.yml`. Doable but vendor code you don't want to maintain long-term.

### 2.8 GT911 touch — LOW IMPACT

`esp_lcd_touch_gt911` 1.2.0~2 in the lock file; public API unchanged. The only related v6.0 change: I2C NACK now returns `ESP_ERR_INVALID_RESPONSE` (was `ESP_ERR_INVALID_STATE`). p3a's `components/app_touch` reads via the driver — no direct I2C calls — so this won't propagate unless the driver itself was checking for the old error code (unlikely).

### 2.9 USB / TinyUSB — LIKELY, possible bump

- The core `usb` component moved to the registry — `idf.py add-dependency "espressif/usb"` if reaching `usb/usb_host.h` directly (p3a doesn't — TinyUSB device mode only).
- `esp_tinyusb` 1.7.6~2 → likely need a bump to **2.0.x** for v6.0 builds. API surface for MSC + CDC + vendor endpoints (the PICO-8 stream) should remain stable, but **read the esp_tinyusb 2.0.0 release notes carefully** — major version bumps from this component family have historically had subtle device-init reorderings.
- `esp_vfs_cdcacm.h` moved to `esp_usb_cdc_rom_console`. p3a doesn't include it, so no edit needed.

### 2.10 Storage (LittleFS, SDMMC, FATFS) — OPTIONAL bump

- `joltwallet/littlefs` 1.20.3 — public API unchanged on v6.0; bump to **1.21.1** opportunistically.
- `esp_vfs_fat_sdmmc_unmount` renamed to `esp_vfs_fat_sdcard_unmount`. p3a doesn't call it (grep clean). The Waveshare BSP and/or `app_usb.c` go through SDMMC card APIs that the BSP wraps.
- FATFS defaults to dynamic buffers + heap-based LFN — could change memory pressure slightly. Probably fine on the ESP32-P4's PSRAM.

### 2.11 OTA — OPTIONAL rename pass

`esp_ota_get_app_description()` → `esp_app_get_description()`. `esp_ota_get_app_elf_sha256()` → `esp_app_get_elf_sha256()`. Grep found **no direct hits** in p3a, but `components/slave_ota/slave_ota.c` works with `esp_app_desc_t` and may call them indirectly via newer wrappers. Worth scanning when touching that file.

### 2.12 SNTP / lwIP — NO IMPACT

`sntp.h` → `esp_sntp.h` rename. Grep found no `#include "sntp.h"` in p3a — `components/wifi_manager/sntp_sync.c` already uses the correct modern header (otherwise warnings would have surfaced on v5.5).

### 2.13 FreeRTOS renames — NO IMPACT

`xTaskGetAffinity`, `vTaskDelayUntil`, `xQueueGenericReceive`, etc. — all grep-clean in p3a.

### 2.14 Custom JPEG workarounds — REQUIRES re-evaluation

Two files exist specifically to work around v5.5.x JPEG driver bugs:
- `components/animation_decoder/jpeg_animation_decoder.c` — comment references "IDF v5.5.1 esp_driver_jpeg" SOF gate at `(W*H) % 8 != 0`.
- `components/animation_decoder/idf_jpeg_release_null_fix.c` — workaround for v5.5.2 JPEG release null-pointer bug.

Both **may no longer be needed in v6.0** (the underlying driver has been worked on heavily) — but they also **may collide with v6.0's driver internals**, particularly the null-fix shim that monkey-patches a release function. After migration, retest with both workarounds disabled and re-enable only what's still needed.

Likewise: `components/art_institution/museums/rijksmuseum.c` has a comment about v5.5.2 `http_client` returning `NULL` on 3xx — verify in v6.0.

### 2.15 Build system — MINOR

`CMakeLists.txt` carries a workaround for the `idf::libjpeg-turbo` bare-name alias (a v5.5.x component-aliasing quirk). Almost certainly unnecessary in v6.0; verify and remove. `esp-idf-kconfig` upgraded to v3 may shake out subtle Kconfig syntax issues — unlikely to hit p3a's custom `Kconfig.projbuild` files but worth re-running `idf.py menuconfig` once.

---

### Refactoring effort estimate

Assuming the Waveshare BSP ships a v6.0-compatible release:

| Tier | Items | Estimated effort |
|------|-------|------------------|
| Mechanical | `psa_crypto_init` add, `mqtt` dep add, `esp_wifi_remote` bump, silicon-rev sdkconfig, OTA-rename pass | **~2–4 hours** |
| Audit | TLS endpoint check, JPEG workaround re-evaluation, warnings-as-errors pass on all 26 components | **~1–3 days** |
| Risk | Vendor BSP update or local fork, full functional re-test (Wi-Fi, MQTT, OTA, MIPI display, touch, USB MSC, GIF/JPEG/PNG decode paths, museum/Giphy/Makapix integrations) | **~3–7 days** |

Realistic total: **1–2 weeks of focused work**, assuming no blocking issues from Waveshare.

---

## 3. Pros, Cons, Risks

### Pros
- **Smaller binaries** (~20% via Picolibc) — meaningful headroom on 8 MB OTA slots given the firmware is growing.
- **GCC 15** — better diagnostics, modern C/C++ standards, more constexpr support if C++ components are ever introduced.
- **Modern crypto baseline** — mandatory PSA Crypto and removal of weak cipher suites is a real security improvement for a device that talks to several internet services.
- **ESP32-P4 v3.0 silicon officially supported** — future-proofs the project against newer Waveshare board revisions.
- **Cleaner API surface** — many long-deprecated APIs are finally gone, which makes future maintenance easier (less "which API should I use?" friction).
- **Component ecosystem alignment** — managed components are increasingly the canonical way to consume optional features (the `mqtt` move is symptomatic). Being on v6.0 keeps p3a in the mainstream.
- **Eventually unavoidable** — v5.5 LTS support ends in 2027.

### Cons
- **No new feature in v6.0 directly unblocks anything p3a is currently doing**. This is a maintenance migration, not a capability migration.
- **v5.5.4 LTS just shipped (2026-05-08)** — staying on v5.5 is fully supported and pulls bug fixes for another year. Migration urgency is low.
- **Custom JPEG workarounds** are a sunk-cost that would have to be revalidated without payoff if the underlying bugs are already fixed.
- **Vendor BSP risk** is real — Waveshare's release cadence is independent and historically lags. The BSP is the most fragile dependency.
- **Effort/benefit ratio is unfavorable for a hobby/personal project** unless the smaller binaries are specifically wanted or features that v6.0 enables are planned.

### Risks
- **Hard blocker risk: Waveshare BSP** — if there's no v6.0-compatible BSP release when migration starts, the choice is wait or maintain a local fork. The latter is non-trivial because the BSP encapsulates the MIPI-DSI panel init and that's where most v6.0 LCD API breakage lands.
- **Manageable: esp_hosted is not a v6.0 blocker.** 2.9.7 builds against v6.0 fine (manifest is `idf: ">=5.3"`, no upper bound), so the migration doesn't force a bump. The reason to **not** bump (intentional ~47.6 KB DMA-internal SRAM preallocation in 2.12.6+, see [esp-hosted-mcu#191](https://github.com/espressif/esp-hosted-mcu/issues/191)) is independent of v6.0 and tracked separately.
- **TLS endpoint regressions** — removed CAs and dropped cipher suites silently break connections. Affects all four external services (Makapix, Giphy, museums, GitHub).
- **PSA Crypto memory footprint** — v6.0 increases PSA's RAM use; p3a is already heavy on PSRAM (`CONFIG_SPIRAM_FLASH_LOAD_TO_PSRAM=y`, `CONFIG_SPIRAM_RODATA=y`). Probably absorbable, worth measuring.
- **Picolibc edge cases** — global stdio could cause issues for any code that assumed Newlib's per-task semantics. p3a probably doesn't, but it's a category of subtle bug.
- **Migration guide gaps** — the community has flagged sections of the official migration guide as incomplete (issue #18382). Expect to dig through GitHub issues to resolve specific build errors.
- **Warnings-as-errors will surface real bugs** — that's a pro, but in the short term it's labor.
- **JPEG workaround interactions** — the shim that monkey-patches `esp_driver_jpeg`'s release function could fail at link time or runtime against v6.0's driver. May require either deletion or rewriting.

---

## 4. Recommendation

**Hold on v5.5.x for production. Install v6.0.1 in parallel on this workstation now for exploration.** Concretely:

1. Install ESP-IDF v6.0.1 via EIM into `C:\Espressif\` so three versions sit side-by-side.
2. Branch `migration/idf-6.0` from `main`. On that branch, do the mechanical changes from §2.1, §2.4, §2.5 (§2.6 is unchanged — no `esp_hosted` bump) and try `idf.py build` with `CONFIG_COMPILER_DISABLE_DEFAULT_ERRORS=y` to see how far it gets. This is a 1-day investment that gives a concrete picture of the actual blockers.
3. Wait for either v6.0.2 (2026-06-04) or a confirmed v6.0-compatible Waveshare BSP release — whichever is later — before committing to the migration.
4. The other workstation on v5.5.2 is irrelevant; both v5.5.1 and v5.5.2 build the same source, so cross-machine consistency isn't a migration driver.

---

## 5. Open Questions

1. **What does Waveshare's component-registry page show for the BSP today?** This determines whether a migration is viable at all in the short term.
2. **What ESP32-P4 silicon revision is on the board?** (Check `idf.py monitor` boot banner, look for `chip revision: vX.Y`.) This decides which `CONFIG_ESP32P4_REV_*` is needed.
3. **Is there a feature in v6.0 specifically worth chasing**, or is this purely a "should we keep current" question? The recommendation flips to "migrate sooner" if there's a concrete v6.0 capability wanted (e.g., binary-size reduction for room to grow, or specific new managed components).
4. **Do a §2 mechanical-changes pass on a branch now** to see actual build output, rather than reasoning abstractly? ~30–60 minutes of edits, then hand back for build.
5. ~~**Check the `esp_hosted` upstream issue tracker** for the status of the SDIO RX mempool regression?~~ ✅ Resolved 2026-05-16 — see §2.6. Not a v6.0 blocker; the 2.9.7 pin stays in place across the migration.

---

## Sources

### ESP-IDF v6.0 official
- [Announcing ESP-IDF v6.0 — Espressif Developer Portal](https://developer.espressif.com/blog/2026/03/idf-v6-0-release/)
- [ESP-IDF v6.0 release notes — GitHub](https://github.com/espressif/esp-idf/releases/tag/v6.0)
- [Espressif Announces ESP-IDF v6.0](https://www.espressif.com/en/news/ESP_IDF_6.0)
- [Migration from 5.5 to 6.0 — Programming Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/index.html)
- [Wi-Fi migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/wifi.html)
- [Networking migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/networking.html)
- [Peripherals migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/peripherals.html)
- [Protocols migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/protocols.html)
- [Security migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/security.html)
- [System migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/system.html)
- [Storage migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/storage.html)
- [Build system migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/build-system.html)
- [Toolchain migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/toolchain.html)
- [Tools migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-6.x/6.0/tools.html)
- [ESP32-P4 Wi-Fi Expansion (ESP-Hosted)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/wifi-expansion.html)
- [ESP-IDF Versions and Support Policy](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/versions.html)
- [ESP-IDF Roadmap (2026 release schedule)](https://github.com/espressif/esp-idf/blob/master/ROADMAP.md)

### Installation
- [ESP-IDF Installation Manager (EIM)](https://docs.espressif.com/projects/idf-im-ui/en/latest/)
- [EIM downloads](https://dl.espressif.com/dl/eim/)
- [Windows install guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/windows-setup.html)

### Issue tracker
- [Upcoming breaking changes in IDF v6.0 (Issue #17052)](https://github.com/espressif/esp-idf/issues/17052)
- [Can't build under 6.0 (Issue #18368)](https://github.com/espressif/esp-idf/issues/18368)
- [Lack of info in 6.x Migration Guide (Issue #18382)](https://github.com/espressif/esp-idf/issues/18382)

### Managed components
- [esp_wifi_remote on Component Registry](https://components.espressif.com/components/espressif/esp_wifi_remote)
- [esp_lcd_touch_gt911 on Component Registry](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911)

### Background
- [Transparent Wi-Fi connectivity for non-Wi-Fi ESP32 chips (esp_wifi_remote blog)](https://developer.espressif.com/blog/2025/09/esp-wifi-remote/)
- [ESP-IDF 6.0 Released: Biggest Changes for ESP32 Developers (community summary)](https://esp32.co.uk/esp-idf-6-0-released-biggest-changes-for-esp32-developers/)
- [Espressif's ESP-IDF 6.0 Brings a New Installation Manager (Hackster.io)](https://www.hackster.io/news/espressif-s-esp-idf-6-0-brings-a-new-installation-manager-mcp-server-and-a-new-standard-c-library-b4258de71662)
