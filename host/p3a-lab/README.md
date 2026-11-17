# p3a-lab

Drives the p3a 2.0 dev unit from the workstation: builds, flashes, reads the
UART without resetting the board, and talks to the firmware's test console.
The device tests in `tests/device/` are built on it. Strategy and
conventions: `docs/2.0/TESTING.md`.

```powershell
python host/p3a-lab/lab.py up                # test build, flash, wait for the console, print identity
python host/p3a-lab/lab.py build release     # build one configuration and run the build checks
python host/p3a-lab/lab.py flash test
python host/p3a-lab/lab.py erase             # erase the whole flash
python host/p3a-lab/lab.py boot              # reset and print the boot log
python host/p3a-lab/lab.py cmd version       # one test-console command
python -m pytest tests/device                # flash the test build and run the device tests
python -m pytest tests/device --no-flash     # test what the unit already runs
python -m pytest host/p3a-lab/test_lab.py    # the lab's own unit tests, no device needed
```

Needs Python with `pyserial` and `pytest`, PowerShell 7 (`pwsh`), and the
ESP-IDF 5.5.4 EIM install. `P3A_PORT` overrides the serial port (default: the
only CH343 bridge); `P3A_IDF_PROFILE` overrides the IDF activation script.

## What it guarantees

- **Builds.** `release` builds into `build/` from `sdkconfig.defaults`;
  `test` builds into `build-test/` with `sdkconfig.test.defaults` on top. A
  generated `sdkconfig` older than its defaults files is regenerated. Every
  build is followed by `tools/check_tree.py --build`, so a build that lost
  the silicon-revision setting, outgrew its slot, or mixed test hooks into a
  release fails here.
- **Serial.** The port is opened with DTR and RTS low, so attaching does not
  reset the board. `Uart.reset()` pulses RTS to reset it on purpose. Every
  line is logged with a timestamp under `runs/` (not tracked).
- **Crash detection.** `Uart.panics()` finds panics, asserts, watchdog
  reports and watchdog resets in the log. The pytest `uart` fixture fails
  any test during which one appears.
- **The right board.** `bring_up()` refuses a unit whose MAC is not the dev
  unit's, or whose firmware version is not the tree's.
- **Secrets.** `load_secrets()` reads `%USERPROFILE%\.p3a-lab\secrets.json`.
  Its values are redacted from the UART log and from command echoes. The
  device's console does not echo input.

## Traps

- `idf.py` refuses to run under an MSYS environment and still exits 0. The
  lab strips the `MSYSTEM` variables before calling it; do the same in any
  new wrapper started from Git Bash.
- Flashing needs the serial port free. Close the `Uart` first.
- After a reset, a stray byte can sit in the device's receive buffer and get
  the first command ignored. `Uart.wait_ready()` sends an empty line to
  flush it.
- The USB cable is fragile. A reset with a lost ROM banner and an "Access is
  denied" serial error is the cable, not the firmware.

## The test console

Protocol and commands: `components/p3a_test_hooks/include/p3a_test_hooks.h`.
Commands so far: `version`, `reboot`. Provisioning (Wi-Fi credentials,
device name, API keys) arrives with the settings store and Wi-Fi in M1.4 and
M1.5; the lab side (`load_secrets`, redaction) is already here.
