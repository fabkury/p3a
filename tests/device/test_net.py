# SPDX-License-Identifier: Apache-2.0
"""The network layer on the real device and the real Wi-Fi: joining, the
clock, the name on the LAN, rejoining after a drop, and setup when nothing
is stored.

Credentials come from the lab's secrets file and reach the unit over the
UART. The module leaves the unit provisioned and online.
"""
import re
import socket
import time

import pytest

import lab

pytestmark = pytest.mark.live


def reboot(uart) -> list[str]:
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    return uart.lines[start:]


@pytest.fixture
def online(uart):
    """The unit, on the test rig's Wi-Fi."""
    lab.provision(uart)
    return uart


@pytest.fixture(scope="module", autouse=True)
def leave_online(bring_up):
    yield
    lab.provision(bring_up[0])


def test_boot_does_not_wait_for_the_network(online):
    boot = reboot(online)
    text = "\n".join(boot)
    assert "boot: network failed" not in text
    # The console answers before the address arrives: joining is in the background.
    status = online.command("net status")
    assert status["has_credentials"] is True
    assert status["state"] in ("connecting", "up")
    assert lab.wait_online(online)["up"] is True


def test_joins_and_reports_an_address(online):
    status = lab.wait_online(online)
    assert status["state"] == "up"
    assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+", status["ip"]) and status["ip"] != "0.0.0.0"
    assert status["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"
    assert -95 <= status["rssi"] < 0, "a plausible signal strength in dBm"
    assert status["ssid"] == lab.load_secrets()["wifi"]["ssid"]


def test_the_password_never_appears_in_what_the_device_prints(online):
    password = lab.load_secrets()["wifi"]["password"]
    start = online.mark()
    reboot(online)
    lab.wait_online(online)
    online.command("net status")
    # The lab replaces a secret with "***" in every line it records, so a
    # device line carrying that marker is a device that printed the password.
    for line in online.lines[start:]:
        assert password not in line
        assert "***" not in line or "[lab]" in line, line


def test_one_up_event_per_connection(online):
    reboot(online)
    status = lab.wait_online(online)
    assert status["up_events"] == 1 and status["down_events"] == 0


def test_the_internet_resolves(online):
    lab.wait_online(online)
    reply = online.command("net resolve example.com", timeout=20)
    assert reply["resolved"] is True and re.fullmatch(r"\d+\.\d+\.\d+\.\d+", reply["ip"])


def test_the_clock_is_set_from_the_network(online):
    reboot(online)
    early = online.command("clock")
    lab.wait_online(online)
    deadline = time.monotonic() + 30
    clock = online.command("clock")
    while not clock["set"] and time.monotonic() < deadline:
        time.sleep(1)
        clock = online.command("clock")
    assert clock["set"] is True, "SNTP should answer within 30 s of joining"
    assert abs(clock["unix"] - time.time()) < 10, "within 10 s of the workstation's clock"
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", clock["now"])
    assert online.command("net status")["time_set"] is True
    if not early["set"]:
        assert early["now"] == "" and early["error"] == "ESP_ERR_INVALID_STATE", "no 1970 timestamps"


def test_the_workstation_can_reach_the_unit_at_its_address(online):
    status = lab.wait_online(online)
    # Nothing listens on port 80 until the HTTP server arrives, so a refused
    # connection is the proof that the unit is there and answering.
    try:
        socket.create_connection((status["ip"], 80), timeout=5).close()
        reached = True
    except ConnectionRefusedError:
        reached = True
    except OSError:
        reached = False
    assert reached, "the unit's address did not answer at all (timeout or unreachable)"


def test_it_rejoins_by_itself_after_a_drop(online):
    before = lab.wait_online(online)
    online.command("net drop")
    time.sleep(0.5)
    after = lab.wait_online(online, timeout=30)
    assert after["drops"] == before["drops"] + 1
    assert after["down_events"] == before["down_events"] + 1
    assert after["up_events"] == before["up_events"] + 1
    assert after["reinits"] == before["reinits"], "one drop is not worth a driver re-initialisation"
    assert after["ip"] != ""


def test_the_device_name_setting_becomes_the_hostname(online):
    # The bare name "p3a" is never tried on this LAN: another device owns it.
    online.command('settings set {"device_name":"lab"}')
    reboot(online)
    try:
        assert lab.wait_online(online)["hostname"] == "p3a-lab"
    finally:
        online.command("settings set", {"device_name": lab.DEV_UNIT_NAME})
        reboot(online)
    assert lab.wait_online(online)["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"


def test_the_unit_answers_to_its_name_on_the_lan(online):
    status = lab.wait_online(online)
    deadline = time.monotonic() + 20
    resolved = None
    while resolved is None and time.monotonic() < deadline:
        try:
            resolved = socket.gethostbyname(f"{status['hostname']}.local")
        except OSError:
            time.sleep(2)
    assert resolved == status["ip"], "mDNS: the workstation resolves <hostname>.local to the unit"


def test_bad_credentials_are_refused_without_being_stored(online):
    for bad in ({"ssid": "", "password": "x"}, {"ssid": "s" * 33, "password": "x"},
                {"ssid": "ok", "password": "p" * 65}, {"ssid": "ok"}):
        with pytest.raises(lab.LabError):
            online.command("net join", bad)
    reboot(online)
    assert lab.wait_online(online)["ssid"] == lab.load_secrets()["wifi"]["ssid"]


def test_with_nothing_stored_the_device_asks_for_setup(online):
    online.command("net forget")
    boot = reboot(online)
    assert any("needs setting up" in line for line in boot)
    status = online.command("net status")
    assert status["state"] == "setup" and status["has_credentials"] is False
    assert status["ip"] == "" and status["up"] is False
    time.sleep(3)
    assert online.command("net status")["up_events"] == 0


def test_a_network_that_is_not_there_ends_in_setup(online):
    online.command("net join", {"ssid": "p3a-lab-no-such-network", "password": "not-a-real-password"})
    reboot(online)
    deadline = time.monotonic() + 60
    status = online.command("net status")
    while status["state"] == "connecting" and time.monotonic() < deadline:
        time.sleep(2)
        status = online.command("net status")
    assert status["state"] == "setup", "after a few tries the device gives up and asks for setup"
    assert status["up_events"] == 0
