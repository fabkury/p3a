# SPDX-License-Identifier: Apache-2.0
"""The unit boots the test build of this tree and its test console answers."""
import pytest

import lab


def test_identity_matches_the_tree(identity):
    assert identity["version"] == lab.project_version()
    assert identity["mac"] == lab.DEV_UNIT_MAC
    assert identity["chip_rev"] == 100, "the board's ESP32-P4 is silicon rev v1.0"
    assert identity["cores"] == 2
    assert identity["idf"].startswith("v5.5")


def test_version_command(uart, identity):
    assert uart.command("version") == identity


def test_unknown_command_is_an_error(uart):
    with pytest.raises(lab.LabError, match="unknown command"):
        uart.command("no-such-command")


def test_overlong_line_is_rejected_and_the_console_survives(uart):
    with pytest.raises(lab.LabError, match="line too long"):
        uart.command("version", {"padding": "x" * 2400})
    assert uart.command("version")["version"] == lab.project_version()


def test_plain_text_is_ignored(uart):
    start = uart.mark()
    uart._ser.write(b"hello, not a command\n")
    assert uart.command("version")["version"] == lab.project_version()
    assert not any("TEST|err" in line for line in uart.lines[start:])


def test_reboot_command_brings_the_console_back(uart):
    start = uart.mark()
    assert uart.command("reboot") == {}
    ident = uart.wait_ready(since=start)
    assert ident["version"] == lab.project_version()
    boot = uart.lines[start:]
    assert any("boot complete" in line for line in boot)
    assert any("TEST BUILD" in line for line in boot), "a test build must say so in its log"
