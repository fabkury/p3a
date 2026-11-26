# SPDX-License-Identifier: Apache-2.0
"""Formatting the SD card on the device, with scripted fingers.

The steps are host-tested (ui_card). Here they run on the device: the offer
on the information screen once the card has failed, the warning with its
dead confirm button, cancelling, and, in the last test, a real format of
the dev unit's card followed by the restart into a clean card.

The dev unit's card is erased by the last test. It belongs to the
development work and every test puts its own files on it.
"""
import time

import pytest

import lab
from test_api import Api

pytestmark = [pytest.mark.live, pytest.mark.slow]

LIFT = [0, 0, 0, 0, 0, 60]


def ui(api) -> dict:
    return api.get("/api/v2/ui").data


def wait_ui(api, what: str, test, timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    state = ui(api)
    while not test(state) and time.monotonic() < deadline:
        time.sleep(0.2)
        state = ui(api)
    assert test(state), f"{what}: {state}"
    return state


def run(uart, script: list) -> None:
    assert uart.command("ui touch", script) == {"result": "ok"}
    deadline = time.monotonic() + sum(step[5] for step in script) / 1000 + 3
    while uart.command("ui touched")["scripted"] and time.monotonic() < deadline:
        time.sleep(0.1)
    time.sleep(0.2)


def tap_button(uart, api, label: str) -> None:
    button = next(b for b in ui(api)["buttons"] if b["label"] == label)
    run(uart, [[1, button["x"] + button["w"] // 2, button["y"] + button["h"] // 2, 0, 0, 80], LIFT])


def long_press(uart) -> None:
    run(uart, [[1, 360, 200, 0, 0, 2400], LIFT])


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    uart.command("settings set", {"rotation": 0})
    yield uart, api
    # Whatever happened, the next tests get a unit with its card in service.
    if uart.command("storage info")["health"] != "ok":
        start = uart.mark()
        uart.command("reboot")
        uart.wait_ready(since=start, timeout=30)
        lab.provision(uart)


def to_the_warning(uart, api) -> dict:
    """Latches the card as failed and walks to the warning through the information screen."""
    for _ in range(4):
        if uart.command("storage latch")["failed"]:
            break
    assert "buttons" not in ui(api) or ui(api)["screen"] != "info"
    long_press(uart)
    info = wait_ui(api, "the information screen offers the format", lambda s: s["screen"] == "info" and "buttons" in s)
    assert info["buttons"][0]["label"] == "Format SD card" and info["buttons"][0]["enabled"]
    assert info["buttons"][0]["h"] >= 90, "a finger's worth"
    tap_button(uart, api, "Format SD card")
    return wait_ui(api, "the warning is up", lambda s: s["screen"] == "card")


def test_the_warning_can_be_cancelled_and_its_confirm_button_is_dead_at_first(rig):
    uart, api = rig
    warning = to_the_warning(uart, api)
    assert warning["title"] == "Erase the SD card?" and "cannot be undone" in warning["text"]
    confirm, cancel = warning["buttons"]
    assert confirm["label"].startswith("Wait") and confirm["enabled"] is False
    assert cancel == {**cancel, "label": "Cancel", "enabled": True}
    # A tap on the dead button, at once: nothing is erased, the warning stays.
    run(uart, [[1, confirm["x"] + confirm["w"] // 2, confirm["y"] + confirm["h"] // 2, 0, 0, 80], LIFT])
    assert ui(api)["screen"] == "card" and ui(api)["title"] == "Erase the SD card?"
    armed = wait_ui(api, "the button comes alive", lambda s: s["buttons"][0]["label"] == "Erase and format", timeout=6)
    assert armed["buttons"][0]["enabled"] is True

    tap_button(uart, api, "Cancel")
    assert wait_ui(api, "back on the information screen", lambda s: s["screen"] == "info")["screen"] == "info"
    # Again, cancelled by a long press this time.
    tap_button(uart, api, "Format SD card")
    wait_ui(api, "the warning is up again", lambda s: s["screen"] == "card")
    long_press(uart)
    wait_ui(api, "cancelled", lambda s: s["screen"] == "info")
    long_press(uart)  # and the information screen closes
    wait_ui(api, "closed", lambda s: s["screen"] != "info")
    # The card was not touched: its files can still be listed.
    assert uart.command("sched refresh 1") == {"result": "ok"}
    deadline = time.monotonic() + 20
    channel = api.get("/api/v2/player").data["channels"][0]
    while channel["state"] == "listing" and time.monotonic() < deadline:
        time.sleep(0.5)
        channel = api.get("/api/v2/player").data["channels"][0]
    assert channel["state"] == "ready" and "error" not in channel


def test_confirming_formats_the_card_and_restarts_into_a_clean_one(rig):
    uart, api = rig
    marker = "/api/v2/test/local?name=zz-before-format.png"
    # Something on the card to see gone. Written before the latch stops writes.
    if uart.command("storage info")["health"] == "ok":
        from test_decoders import CORPUS
        assert api.call("POST", marker, (CORPUS / "good" / "png" / "one_pixel.png").read_bytes(), timeout=30).status == 200
    if ui(api)["screen"] != "card":
        to_the_warning(uart, api)
    wait_ui(api, "the button comes alive", lambda s: s["buttons"][0]["label"] == "Erase and format", timeout=6)
    start = uart.mark()
    tap_button(uart, api, "Erase and format")
    uart.wait_for(r"formatting the SD card at a person's request", timeout=10, since=start)
    uart.wait_for(r"the card was formatted", timeout=180, since=start)
    uart.wait_ready(since=start, timeout=60)  # the restart the flow ends in

    api = Api(lab.provision(uart)["ip"])
    info = uart.command("storage info")
    assert info["health"] == "ok", "the fresh card is in service"
    assert ui(api)["screen"] != "card"
    uart.command("sched activate", {"name": "", "channels": [{"source": "local"}]})
    uart.command("sched refresh 1")
    time.sleep(2.0)
    player = api.get("/api/v2/player").data
    assert player["channels"][0]["records"] == 0, "nothing of what was on the card is left"
