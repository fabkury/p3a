# Web Flasher Maintenance

This folder keeps the assets required by the Espressif Web Flasher flow as exposed in the main `README.md`.

## Refreshing the binaries

1. Make sure you have `esp-idf v5.5.x` installed and the `esp32p4` target selected.
2. Rebuild the firmware so that the committed binaries under `build/` are up-to-date:
   ```bash
   idf.py build
   ```
3. The artifacts consumed by the flasher live in:
   - `build/bootloader/bootloader.bin`
   - `build/partition_table/partition-table.bin`
   - `build/p3a.bin`
   - `build/storage.bin`
4. Re-run any validation you need (e.g., `esptool.py` flashing on a dev unit) before committing updated binaries back to the repository.

## Updating the manifest

The browser-based flasher reads `docs/web-flasher/p3a-esp32p4.json`. To update it:

1. Use `build/flasher_args.json` as the source of truth for offsets and flash parameters.
2. If offsets or new partitions change, mirror those changes inside `docs/web-flasher/p3a-esp32p4.json`.
3. Keep the file URLs pointing to `https://raw.githubusercontent.com/fabkury/p3a/main/...` so the public tool can fetch the binaries directly from GitHub.
4. After editing, open the flasher link locally to ensure it parses:
   ```
   https://espressif.github.io/web-tools/flash?flash_config_url=https://raw.githubusercontent.com/fabkury/p3a/main/docs/web-flasher/p3a-esp32p4.json
   ```
5. Commit both the refreshed binaries and the manifest update together so they cannot fall out of sync.

