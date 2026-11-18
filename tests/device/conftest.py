# SPDX-License-Identifier: Apache-2.0
"""Fixtures for the device tests (tier T3 in docs/2.0/TESTING.md).

    python -m pytest tests/device                 build the test config, flash, run
    python -m pytest tests/device --no-flash      run against what the unit already has
    python -m pytest tests/device -m "not live"   skip tests that need the internet

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


def pytest_configure(config):
    config.addinivalue_line("markers", "live: needs the internet or a third-party service")
    config.addinivalue_line("markers", "expects_crash: the test crashes the firmware on purpose")


@pytest.fixture(scope="session")
def bring_up(request):
    """The dev unit, flashed with the test build of this tree, console ready."""
    uart, ident = lab.bring_up("test", do_flash=not request.config.getoption("--no-flash"))
    yield uart, ident
    # Leave the unit as the next session expects it: named and on the Wi-Fi.
    try:
        lab.provision(uart)
    except lab.LabError as exc:
        print(f"the dev unit was not left online: {exc}")
    uart.close()


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
