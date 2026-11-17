#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""p3a-lab: drive the p3a 2.0 dev unit from the workstation.

    python host/p3a-lab/lab.py build [release|test]
    python host/p3a-lab/lab.py flash [release|test]      build, then flash
    python host/p3a-lab/lab.py erase                     erase the whole flash
    python host/p3a-lab/lab.py up                        test build, flash, wait for the console, print identity
    python host/p3a-lab/lab.py boot                      reset and print the boot log
    python host/p3a-lab/lab.py cmd <name> [json]         send one test-console command
    python host/p3a-lab/lab.py shot <file.png> [divisor] save what the test hooks last put on the panel
    python host/p3a-lab/lab.py port                      print the dev unit's serial port

Also imported by tests/device/conftest.py. Background: docs/2.0/TESTING.md.

The serial port is opened with DTR and RTS low, which does not reset the
board; plain `idf.py monitor` and .NET SerialPort do. Flashing needs the port
free, so close any Uart before calling flash() or erase().

Secrets (%USERPROFILE%/.p3a-lab/secrets.json) are loaded only by
load_secrets() and are redacted from every log this module writes.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import serial
import serial.tools.list_ports

REPO = Path(__file__).resolve().parents[2]
RUNS = Path(__file__).resolve().parent / "runs"
LAB_HOME = Path(os.environ.get("USERPROFILE", str(Path.home()))) / ".p3a-lab"
IDF_PROFILE = os.environ.get("P3A_IDF_PROFILE", r"C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1")

# The dev unit (docs/2.0/README.md, "Environment"). Tests refuse any other board.
DEV_UNIT_MAC = "30:ED:A0:E2:09:37"
BAUD = 115200


@dataclass(frozen=True)
class BuildConfig:
    name: str
    build_dir: str
    overlays: tuple[str, ...]  # applied after sdkconfig.defaults

    @property
    def sdkconfig(self) -> Path:
        return REPO / self.build_dir / "sdkconfig"


CONFIGS = {
    "release": BuildConfig("release", "build", ()),
    "test": BuildConfig("test", "build-test", ("sdkconfig.test.defaults",)),
    # The jitter work's overlays: release plus the frame trace, and the heavier diagnostic set.
    "trace": BuildConfig("trace", "build-trace", ("sdkconfig.trace.defaults",)),
    "diag": BuildConfig("diag", "build-diag", ("sdkconfig.diag.defaults",)),
}

# A line matching any of these means the firmware crashed or hung.
PANIC_PATTERNS = [
    re.compile(p)
    for p in (
        r"Guru Meditation Error",
        r"abort\(\) was called",
        r"assert failed",
        r"Task watchdog got triggered",
        r"Interrupt wdt timeout",
        r"Stack canary watchpoint triggered",
        r"\*\*\*ERROR\*\*\* A stack overflow",
        r"rst:0x[0-9a-f]+ \((?!POWERON|SW_SYS_RESET|CHIP_POR|SW\b)[A-Z_0-9]*(WDT|PANIC|BROWN)",
        r"Rebooting\.\.\.\s*$",
    )
]


class LabError(RuntimeError):
    pass


def find_panics(lines: list[str]) -> list[str]:
    """The lines that show a crash, a watchdog or an unexpected reset."""
    return [line for line in lines if any(p.search(line) for p in PANIC_PATTERNS)]


# --- secrets ----------------------------------------------------------------

def load_secrets() -> dict:
    """Wi-Fi credentials and API keys for the test rig. Never print the result."""
    path = LAB_HOME / "secrets.json"
    if not path.exists():
        raise LabError(f"{path} is missing; see docs/2.0/TESTING.md")
    return json.loads(path.read_text(encoding="utf-8"))


def secret_values(secrets: dict) -> list[str]:
    """Every string in the secrets file worth redacting (6 characters or more)."""
    found: list[str] = []

    def walk(node, key=""):
        if isinstance(node, dict):
            for k, v in node.items():
                if not k.startswith("_") and k != "captured":
                    walk(v, k)
        elif isinstance(node, str) and len(node) >= 6 and key != "ssid":
            found.append(node)

    walk(secrets)
    return found


# --- toolchain --------------------------------------------------------------

def find_port() -> str:
    """The dev unit's UART: P3A_PORT if set, else the only CH343 bridge."""
    if os.environ.get("P3A_PORT"):
        return os.environ["P3A_PORT"]
    ports = [p.device for p in serial.tools.list_ports.comports() if "CH343" in (p.description or "")]
    if len(ports) != 1:
        raise LabError(f"expected exactly one CH343 serial port, found {ports or 'none'}; set P3A_PORT")
    return ports[0]


def run_idf(args: list[str], cfg: BuildConfig, quiet: bool = True) -> str:
    """Runs idf.py in an activated IDF PowerShell, from the repository root."""
    idf_args = []
    if cfg.name != "release":
        defaults = ";".join(("sdkconfig.defaults",) + cfg.overlays)
        idf_args += ["-B", cfg.build_dir, f"-DSDKCONFIG={cfg.build_dir}/sdkconfig", f"-DSDKCONFIG_DEFAULTS={defaults}"]
    idf_args += args
    quoted = " ".join("'" + a.replace("'", "''") + "'" for a in idf_args)
    script = (
        "$env:PYTHONUTF8='1'; "
        f". '{IDF_PROFILE}' | Out-Null; "
        "$env:ESP_IDF_VERSION='5.5'; "
        f"Set-Location '{REPO}'; "
        f"idf.py {quoted}; exit $LASTEXITCODE"
    )
    # idf.py refuses to run when it sees an MSYS environment (as when this
    # module is started from Git Bash) and still exits 0, so strip the markers.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MSYSTEM", "MINGW", "MSYS"))}
    proc = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-Command", script],
                          capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    out = proc.stdout + proc.stderr
    RUNS.mkdir(exist_ok=True)
    (RUNS / f"idf-{cfg.name}-{args[-1]}.log").write_text(out, encoding="utf-8")
    if proc.returncode != 0:
        tail = "\n".join(out.splitlines()[-40:])
        raise LabError(f"idf.py {' '.join(args)} failed ({cfg.name}):\n{tail}")
    if not quiet:
        print("\n".join(out.splitlines()[-6:]))
    return out


def _generated_sdkconfig(cfg: BuildConfig) -> Path:
    return REPO / "sdkconfig" if cfg.name == "release" else cfg.sdkconfig


def build(config: str = "test") -> Path:
    """Builds one configuration and runs the build checks. Returns the build directory."""
    cfg = CONFIGS[config]
    # Defaults files only apply when sdkconfig is created: regenerate when one is newer.
    generated = _generated_sdkconfig(cfg)
    if generated.exists():
        sources = [REPO / "sdkconfig.defaults"] + [REPO / o for o in cfg.overlays]
        if any(s.stat().st_mtime > generated.stat().st_mtime for s in sources):
            generated.unlink()
    run_idf(["build"], cfg)
    check = [sys.executable, str(REPO / "tools" / "check_tree.py"), "--build", cfg.build_dir]
    if cfg.name == "test":
        check.append("--test-build")
    proc = subprocess.run(check, capture_output=True, text=True)
    if proc.returncode != 0:
        raise LabError(f"tree checks failed for the {cfg.name} build:\n{proc.stdout}{proc.stderr}")
    if cfg.name == "release":
        # The release configuration must still say what 1.x shipped with.
        proc = subprocess.run([sys.executable, str(REPO / "tools" / "compare_sdkconfig.py")],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise LabError(f"the release configuration drifted from 1.x:\n{proc.stdout}{proc.stderr}")
    return REPO / cfg.build_dir


def flash(config: str = "test", port: str | None = None) -> None:
    """Builds if needed and flashes. The serial port must be free."""
    build(config)
    run_idf(["-p", port or find_port(), "flash"], CONFIGS[config])


def erase(port: str | None = None) -> None:
    run_idf(["-p", port or find_port(), "erase-flash"], CONFIGS["release"])


def project_version() -> str:
    text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', text)
    if not m:
        raise LabError("PROJECT_VER not found in CMakeLists.txt")
    return m.group(1)


# --- serial -----------------------------------------------------------------

class Uart:
    """The dev unit's UART: a background reader, a line log, and the test console.

    Every received line is kept in memory and appended, with a timestamp, to
    a log file under host/p3a-lab/runs/. Secrets are redacted in both.
    """

    def __init__(self, port: str | None = None, log_name: str = "uart.log", redact: list[str] | None = None):
        self.port = port or find_port()
        self.redact = sorted(redact or [], key=len, reverse=True)
        self.lines: list[str] = []
        self.last_data: list[str] = []
        self._cond = threading.Condition()
        self._stop = False
        RUNS.mkdir(exist_ok=True)
        self.log_path = RUNS / log_name
        self._log = open(self.log_path, "a", encoding="utf-8")
        self._ser = serial.Serial()
        self._ser.port = self.port
        self._ser.baudrate = BAUD
        self._ser.timeout = 0.1
        self._ser.dtr = False  # both low before open(): no reset, no bootloader entry
        self._ser.rts = False
        self._ser.open()
        # Windows gives a serial port a 4 KB receive buffer by default. A burst
        # (a screenshot is 200 KB) can overflow it whenever this process is
        # held up for a moment, and the bytes are lost silently.
        if hasattr(self._ser, "set_buffer_size"):
            self._ser.set_buffer_size(rx_size=1 << 20)
        self._thread = threading.Thread(target=self._reader, name="uart-reader", daemon=True)
        self._thread.start()

    # -- lifecycle
    def close(self) -> None:
        self._stop = True
        self._thread.join(timeout=2)
        try:
            self._ser.close()
        finally:
            self._log.close()

    def __enter__(self) -> "Uart":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- reading
    def _clean(self, text: str) -> str:
        for secret in self.redact:
            text = text.replace(secret, "***")
        return text

    def _reader(self) -> None:
        pending = b""
        while not self._stop:
            try:
                chunk = self._ser.read(4096)
            except serial.SerialException as exc:
                self._append(f"[lab] serial error: {exc}")
                return
            if not chunk:
                # Idle: a good moment to push the log to disk. Flushing after
                # every line stalls the reader while the folder is being synced.
                with self._cond:
                    self._log.flush()
                continue
            pending += chunk
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                self._append(raw.decode("utf-8", "replace").rstrip("\r"))

    def _append(self, line: str) -> None:
        line = self._clean(line)
        with self._cond:
            self.lines.append(line)
            self._log.write(f"{time.strftime('%H:%M:%S')} {line}\n")
            self._cond.notify_all()

    def mark(self) -> int:
        """A position in the log; pass it as `since` to look only at later lines."""
        with self._cond:
            return len(self.lines)

    def note(self, text: str) -> None:
        """Writes a lab-side remark into the log (not sent to the device)."""
        self._append(f"[lab] {text}")

    def wait_for(self, pattern: str, timeout: float = 10.0, since: int = 0) -> re.Match:
        """Blocks until a line at or after `since` matches; raises LabError on timeout."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        pos = since
        with self._cond:
            while True:
                while pos < len(self.lines):
                    m = rx.search(self.lines[pos])
                    pos += 1
                    if m:
                        return m
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    tail = "\n".join(self.lines[-15:])
                    raise LabError(f"timed out after {timeout}s waiting for /{pattern}/. Last lines:\n{tail}")
                self._cond.wait(remaining)

    def panics(self, since: int = 0) -> list[str]:
        """Lines at or after `since` that show a crash, a watchdog or an unexpected reset."""
        with self._cond:
            window = self.lines[since:]
        return find_panics(window)

    # -- acting
    def reset(self) -> int:
        """Pulses the board's reset line. Returns the log position just before."""
        pos = self.mark()
        self.note("reset")
        self._ser.rts = True
        time.sleep(0.1)
        self._ser.rts = False
        return pos

    def wait_ready(self, timeout: float = 15.0, since: int = 0) -> dict:
        """Waits for the test console's ready line and returns the identity it carries."""
        m = self.wait_for(r"TEST\|ready\|(\{.*\})", timeout, since)
        # A reset can leave a stray byte in the device's receive buffer, which
        # would glue itself to the front of the first command and get that
        # line ignored. An empty line flushes it.
        self._ser.write(b"\n")
        self._ser.flush()
        time.sleep(0.05)
        return json.loads(m.group(1))

    def command(self, name: str, payload: dict | None = None, timeout: float = 5.0) -> dict:
        """Sends one test-console command and returns its JSON reply; raises on TEST|err."""
        line = f"!{name}" + (" " + json.dumps(payload, separators=(",", ":")) if payload is not None else "")
        pos = self.mark()
        self.note(f"-> {self._clean(line)}")
        self._ser.write(line.encode("utf-8") + b"\n")
        self._ser.flush()
        m = self.wait_for(r"TEST\|(ok|err)\|(.*)", timeout, pos)
        if m.group(1) == "err":
            raise LabError(f"command {name!r} failed: {m.group(2)}")
        # Bulk output, if the command produced any, arrives as TEST|data| lines before the reply.
        with self._cond:
            self.last_data = [line.split("TEST|data|", 1)[1] for line in self.lines[pos:] if "TEST|data|" in line]
        return json.loads(m.group(2))

    def screenshot(self, path, divisor: int = 8, timeout: float = 60.0):
        """Saves a downscaled copy of what the test hooks last showed, as a PNG.

        This is the framebuffer, not the glass. divisor must divide 720; 8
        gives 90x90 in about 5 seconds over the UART, 4 gives 180x180 in 20.
        """
        from PIL import Image  # imported here so that the lab works without Pillow

        reply = self.command(f"lcd shot {divisor}", timeout=timeout)
        width, height = reply["width"], reply["height"]
        rows = self.last_data
        if len(rows) != height or any(len(r) != width * 6 for r in rows):
            raise LabError(f"screenshot is incomplete: {len(rows)} rows for {width}x{height}")
        image = Image.frombytes("RGB", (width, height), bytes.fromhex("".join(rows)))
        image.save(path)
        return path


def bring_up(config: str = "test", do_flash: bool = True) -> tuple[Uart, dict]:
    """Flashes (optionally), resets, and waits for the test console.

    Returns the open Uart and the identity the console reported. Refuses a
    board that is not the dev unit, or that runs another version.
    """
    port = find_port()
    if do_flash:
        flash(config, port)
    try:
        redact = secret_values(load_secrets())
    except LabError:
        redact = []
    uart = Uart(port, log_name=f"uart-{time.strftime('%Y%m%d-%H%M%S')}.log", redact=redact)
    try:
        pos = uart.reset()
        ident = uart.wait_ready(since=pos)
        if ident.get("mac") != DEV_UNIT_MAC:
            raise LabError(f"this is not the dev unit: MAC {ident.get('mac')}, expected {DEV_UNIT_MAC}")
        if ident.get("version") != project_version():
            raise LabError(f"the unit runs {ident.get('version')}, the tree is {project_version()}; flash it")
        return uart, ident
    except Exception:
        uart.close()
        raise


# --- command line -----------------------------------------------------------

def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    verb, rest = argv[1], argv[2:]
    try:
        if verb == "port":
            print(find_port())
        elif verb == "build":
            print(build(rest[0] if rest else "test"))
        elif verb == "flash":
            flash(rest[0] if rest else "test")
            print("flashed")
        elif verb == "erase":
            erase()
            print("erased")
        elif verb == "up":
            uart, ident = bring_up("test")
            uart.close()
            print(json.dumps(ident))
        elif verb == "boot":
            with Uart(log_name="boot.log") as uart:
                pos = uart.reset()
                time.sleep(float(rest[0]) if rest else 4.0)
                print("\n".join(uart.lines[pos:]))
        elif verb == "shot":
            with Uart(log_name="cmd.log") as uart:
                print(uart.screenshot(rest[0], int(rest[1]) if len(rest) > 1 else 8))
        elif verb == "cmd":
            with Uart(log_name="cmd.log") as uart:
                print(json.dumps(uart.command(rest[0], json.loads(rest[1]) if len(rest) > 1 else None)))
        else:
            print(__doc__)
            return 2
    except LabError as exc:
        print(f"lab error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
