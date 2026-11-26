# SPDX-License-Identifier: Apache-2.0
"""Fixtures for the device tests (tier T3 in docs/2.0/TESTING.md).

    python -m pytest tests/device                 build the test config, flash, run everything
    python -m pytest tests/device/test_smoke.py tests/device/test_<area>.py
                                                  a checkpoint: the smoke set and the area changed
    python -m pytest tests/device -m "not slow"   everything but the slow tests (a cross-cutting change)
    python -m pytest tests/device --no-flash      run against what the unit already has
    python -m pytest tests/device/test_timing.py --build trace-test
                                                  frame timing; needs the build with the frame trace
    python -m pytest tests/device -m "not live"   skip tests that need the internet

Which of these runs when is in docs/2.0/TESTING.md under "What runs when".

Every test gets the serial log watched: a panic, an assert, a watchdog report
or an unexpected reset during a test fails it, whatever else it asserted.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "p3a-lab"))
import lab  # noqa: E402


def pytest_addoption(parser):
    parser.addoption("--no-flash", action="store_true",
                     help="do not build and flash; test the firmware the dev unit already runs")
    parser.addoption("--build", default="test",
                     help="which build to flash: test (the default) or trace-test (adds the frame trace)")


def pytest_configure(config):
    config.addinivalue_line("markers", "live: needs the internet or a third-party service")
    config.addinivalue_line("markers", "expects_crash: the test crashes the firmware on purpose")
    config.addinivalue_line("markers", "slow: takes 20 s or more, mostly waiting on a real timer or a restart")


# The settings that hold a service's key, and what the lab's secrets file calls each.
API_KEYS = {"giphy_api_key": "giphy_api_key", "klipy_api_key": "klipy_api_key",
            "harvard_api_key": "ham_api_key", "smithsonian_api_key": "si_api_key"}


def put_keys_back(ip: str, wait_s: float = 0.0) -> None:
    """Sets the dev unit's API keys from the lab's secrets file. Tests clear them (to see what a
    device without one does) and reset the unit; a unit left without its keys plays no Giphy,
    Klipy, Harvard or Smithsonian channel until someone types them again. Over HTTP and not the
    test console, so that no key is written into the UART log. `wait_s`: how long to keep trying,
    for a unit that is restarting."""
    import json
    import time
    import urllib.request
    try:
        stored = lab.load_secrets().get("api_keys", {})
    except lab.LabError as exc:
        print(f"the dev unit's API keys were NOT put back: {exc}")
        return
    keys = {setting: stored[name] for setting, name in API_KEYS.items() if stored.get(name)}
    if not keys:
        return
    deadline = time.monotonic() + wait_s
    while True:
        try:
            request = urllib.request.Request(f"http://{ip}/api/v2/settings", data=json.dumps(keys).encode(),
                                             method="PATCH", headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=10) as reply:
                json.load(reply)
            print(f"the dev unit's API keys were put back ({len(keys)})")
            return
        except (OSError, ValueError) as exc:
            if time.monotonic() >= deadline:
                print(f"the dev unit's API keys were NOT put back: {type(exc).__name__}")  # never the request
                return
            time.sleep(3)


@pytest.fixture(scope="session")
def bring_up(request):
    """The dev unit, flashed with the test build of this tree, console ready."""
    uart, ident = lab.bring_up(request.config.getoption("--build"),
                               do_flash=not request.config.getoption("--no-flash"))
    yield uart, ident
    # Leave the unit as the next session expects it: named and on the Wi-Fi.
    registered = None
    ip = None
    try:
        status = lab.provision(uart)
        ip = status["ip"]
        import json
        import urllib.request
        with urllib.request.urlopen(f"http://{status['ip']}/api/v2/makapix", timeout=10) as reply:
            registered = json.load(reply)["data"]["registered"]
    except (lab.LabError, OSError, KeyError, ValueError) as exc:
        print(f"the dev unit was not left online: {exc}")
    uart.close()
    # And registered with Makapix Club, if it was: the factory-reset tests erase the registration
    # with everything else, and only its owner can make another. `lab.py nvs save` keeps the
    # unit's whole memory in the lab's private folder; this writes it back when it was lost.
    if registered is False and lab.nvs_path("registered").exists():
        try:
            lab.nvs_restore("registered")
            print("the dev unit's Makapix registration was put back from the lab's snapshot")
            if ip is not None:
                put_keys_back(ip, wait_s=90)  # it restarts with the snapshot's settings, whatever keys those held
            return
        except lab.LabError as exc:
            print(f"the dev unit's Makapix registration was NOT put back: {exc}")
    # And with its API keys: the Giphy, Klipy and museum tests clear them when they finish.
    if ip is not None:
        put_keys_back(ip)


@pytest.fixture(scope="session")
def identity(bring_up) -> dict:
    """What the unit reported when its test console started."""
    return bring_up[1]


@pytest.fixture
def uart(bring_up, request):
    """The dev unit's UART for one test, with crash detection."""
    link = bring_up[0]
    link.note(f"=== {request.node.nodeid}")
    start = link.mark()
    yield link
    if request.node.get_closest_marker("expects_crash"):
        return
    crashed = link.panics(since=start)
    if crashed:
        pytest.fail("the firmware crashed or reset during this test:\n" + "\n".join(crashed[:10])
                    + f"\nfull log: {link.log_path}", pytrace=False)
