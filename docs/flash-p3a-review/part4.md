# Part 4 — Advanced: Build from Source

Covers `docs/flash-p3a.md` lines 140–161:
- Build-from-source instructions
- Build output paths (`release/v{VERSION}/`, `p3a-flasher.exe`)
- `P3A_BUILD_FLASHER=OFF` flag
- ESP-IDF v5.5.x requirement

## Verification findings

### 1. Clone URL and basic build commands (lines 144–150) — CORRECT

- Repo URL `https://github.com/fabkury/p3a.git` matches the project (root `README.md`, `CLAUDE.md`).
- `idf.py set-target esp32p4` is necessary: there is no `sdkconfig.defaults` that pins the target (only a generated `sdkconfig` exists at the repo root). A fresh checkout therefore requires the explicit `set-target`.
- `idf.py build` is the standard build command (matches `CLAUDE.md`).

### 2. "The build automatically creates `release/v{VERSION}/`" (line 153) — CORRECT (with a naming nit)

- Evidence: `CMakeLists.txt:54` sets `RELEASE_SUBDIR "release/v${PROJECT_VER}"`, and `CMakeLists.txt:433-439` registers a `POST_BUILD` custom command on `gen_project_binary` that runs `create_release.py`, which writes binaries, `.sha256` files, `flash_args`, `flash_command*.txt`, `README.md`, and `manifest.json` into that directory (`CMakeLists.txt:121-417`).
- Naming nit: the variable in `CMakeLists.txt` is `PROJECT_VER` (not `VERSION`). The doc's literal placeholder `{VERSION}` is fine for users, but `CLAUDE.md`'s mention of `set(VERSION "0.6.5-dev")` is stale — actual line 14 reads `set(PROJECT_VER "0.9.0")` (no `-dev` suffix as of this audit). Current release path is therefore `release/v0.9.0/`.

### 3. "`release/v{VERSION}/p3a-flasher.exe` … if built on Windows" (line 154) — INCORRECT (misleading)

The doc implies the flasher exe is produced automatically as long as you're on Windows. In reality it is gated by **two** conditions (`CMakeLists.txt:442`):

```
if(P3A_BUILD_FLASHER AND CMAKE_HOST_WIN32)
```

and `P3A_BUILD_FLASHER` defaults to **OFF** (`CMakeLists.txt:38`: `set(P3A_BUILD_FLASHER OFF)`). On a default Windows build the message printed is:

> Flasher build: DISABLED (set -DP3A_BUILD_FLASHER=ON to enable)
> (`CMakeLists.txt:460`)

So `p3a-flasher.exe` is **not** produced by a default `idf.py build`, even on Windows. It is only built when the user explicitly passes `-DP3A_BUILD_FLASHER=ON` on Windows. The flasher itself is built by `flasher/build_flasher.py` invoked from `CMakeLists.txt:446-455`.

Suggested replacement wording for lines 152–154:

> The build automatically creates:
> - `release/v{VERSION}/` — Firmware files for distribution (binaries, SHA256 checksums, `flash_args`, `flash_command.txt`, `manifest.json`, `README.md`)
>
> On Windows, you can additionally build the standalone `p3a-flasher.exe` (with embedded firmware) by passing `-DP3A_BUILD_FLASHER=ON`:
> ```bash
> idf.py build -DP3A_BUILD_FLASHER=ON
> ```

### 4. "To disable flasher building during development: `idf.py build -DP3A_BUILD_FLASHER=OFF`" (lines 156–159) — INCORRECT

The flasher is **already disabled by default** (`CMakeLists.txt:38`). The `=OFF` invocation is a no-op. The documented flag is correctly spelled (`P3A_BUILD_FLASHER`, all caps, underscores), but the polarity in the doc is reversed: users actually need `-DP3A_BUILD_FLASHER=ON` to enable it, not `=OFF` to disable it.

Suggested correction: remove this snippet, or replace it with the inverse — "To enable flasher building (Windows only): `idf.py build -DP3A_BUILD_FLASHER=ON`".

### 5. "Requires ESP-IDF v5.5.x" (line 161) — CORRECT (loosely)

- `dependencies.lock:190-193` pins `idf` to `version: 5.5.1`.
- `README.md:161` advertises the framework as "ESP-IDF v5.5".
- `CLAUDE.md` documents activation snippets for both v5.5.1 and v5.5.2.
- "v5.5.x" is therefore a reasonable description, though the lockfile names 5.5.1 specifically. Optionally tighten to "ESP-IDF v5.5.1 or v5.5.2".

### Summary

| Claim | Status |
|-------|--------|
| Clone URL + `cd p3a` + `set-target esp32p4` + `build` | CORRECT |
| `release/v{VERSION}/` is auto-populated by the build | CORRECT (variable is `PROJECT_VER`, currently `0.9.0`) |
| `p3a-flasher.exe` is produced when built on Windows | INCORRECT — also requires `-DP3A_BUILD_FLASHER=ON`; default is OFF |
| `-DP3A_BUILD_FLASHER=OFF` disables flasher build | INCORRECT — it's already off; the meaningful flag is `=ON` |
| Requires ESP-IDF v5.5.x | CORRECT (lock pins 5.5.1; v5.5.1/5.5.2 both supported) |

Key file references:
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:14` — `PROJECT_VER` definition (currently `0.9.0`)
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:38` — `P3A_BUILD_FLASHER` default (`OFF`)
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:54` — `RELEASE_SUBDIR "release/v${PROJECT_VER}"`
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:121-417` — release helper script (binaries, SHA256, manifest, README)
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:433-439` — POST_BUILD hook that runs the release script
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\CMakeLists.txt:442-462` — `P3A_BUILD_FLASHER AND CMAKE_HOST_WIN32` gate
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\flasher\build_flasher.py` — script invoked to build `p3a-flasher.exe`
- `D:\Dropbox\PC\F\Estudo\Tecnologia\ESP32\p3a\repo\dependencies.lock:190-193` — IDF version pin (`5.5.1`)
