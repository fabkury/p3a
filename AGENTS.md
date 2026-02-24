You are a highly proficient embedded systems developer with expertise in **ESP-IDF** and **ESP32-P4**. The project is p3a, a Wi-Fi pixel art player. When using idf.py, activate the ESP-IDF environment with `C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1` once and reuse the session to avoid redundant shells. On Windows, set `$env:PYTHONUTF8="1"` to prevent Unicode issues. Do NOT build the project unless directly and explicitly requested by the user. The user will be doing the building and testing.

## Cursor Cloud specific instructions

### ESP-IDF Environment

- ESP-IDF v5.5.1 is installed at `~/esp/v5.5.1/`.
- Activate the environment before any `idf.py` command: `source ~/esp/v5.5.1/export.sh`
- The shell environment persists within a session, so you only need to source once per session.

### Building

- Target is already set to `esp32p4` via `sdkconfig`. No need to run `idf.py set-target`.
- Build with: `source ~/esp/v5.5.1/export.sh && cd /workspace && idf.py build`
- This is an embedded firmware project — there is no way to "run" the application without physical ESP32-P4 hardware. A successful build is the primary validation step in this cloud environment.

### Certificate Setup (required before first build)

The Makapix CA certificate must be placed at `components/makapix/certs/makapix_ca_cert.inc` for the build to succeed. Copy it from the repo's shared certs:

```bash
mkdir -p /workspace/components/makapix/certs
cp /workspace/certs/player/makapix_ca.inc /workspace/components/makapix/certs/makapix_ca_cert.inc
```

### Linting / Testing

- No formal linter (clang-format, clang-tidy) or automated test suite is configured.
- The compiler's `-Werror=all` flag (with specific `-Wno-error` exceptions) enforces code quality at build time.
- Firmware size is checked via `idf.py size`.

### Project Structure

See `CLAUDE.md` for architecture overview, component descriptions, and terminology.