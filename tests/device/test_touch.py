# SPDX-License-Identifier: Apache-2.0
"""Gestures on the device, with scripted fingers: the touch task reads the
script in place of the touch controller, at the controller's pace, so the
path from a sample to what happens is the one real fingers take.

The recogniser and the table are host-tested (ui_gesture, ui_route); this
is the wiring: the task, the rotation, and each action reaching the
scheduler, the settings or the screen model.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = pytest.mark.live

FILES = {"zz-touch-a.gif": "gif/opaque_anim.gif", "zz-touch-b.png": "png/rgb.png", "zz-touch-c.jpg": "jpeg/baseline.jpg"}
LOCAL_ONLY = {"name": "", "channels": [{"source": "local"}]}
UPDATE = 6
LIFT = [0, 0, 0, 0, 0, 60]


def tap(x: int, y: int) -> list:
    return [[1, x, y, 0, 0, 80], LIFT]


def long_press(x: int = 360, y: int = 360) -> list:
    return [[1, x, y, 0, 0, 2400], LIFT]


def drag(x0: int, y0: int, x1: int, y1: int, steps: int = 8) -> list:
    path = [[1, x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps, 0, 0, 30] for i in range(steps + 1)]
    return path + [LIFT]


class Rig:
    def __init__(self, uart, api):
        self.uart, self.api = uart, api

    def fingers(self, script: list) -> dict:
        """Runs a script to its end and says what the last gesture was and did."""
        before = self.uart.command("ui touched")["gestures"]
        assert self.uart.command("ui touch", script) == {"result": "ok"}
        deadline = time.monotonic() + sum(step[5] for step in script) / 1000 + 3
        status = self.uart.command("ui touched")
        while status["scripted"] and time.monotonic() < deadline:
            time.sleep(0.1)
            status = self.uart.command("ui touched")
        assert not status["scripted"], "the script did not finish"
        time.sleep(0.1)
        status = self.uart.command("ui touched")
        status["new"] = status["gestures"] - before
        return status

    def player(self) -> dict:
        return self.api.get("/api/v2/player").data

    def wait_player(self, what: str, test, timeout: float = 10.0) -> dict:
        deadline = time.monotonic() + timeout
        state = self.player()
        while not test(state) and time.monotonic() < deadline:
            time.sleep(0.2)
            state = self.player()
        assert test(state), f"{what}: {state}"
        return state

    def screen(self) -> str:
        return self.api.get("/api/v2/ui").data["screen"]


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))

    def clean():
        for screen in (2, UPDATE):
            uart.command("ui screen", {"screen": screen, "up": False})
        uart.command("sched pause off")
        uart.command("settings set", {"rotation": 0})
        for name in FILES:
            r.api.call("POST", f"/api/v2/test/local?name={name}")

    clean()
    assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    for name, source in FILES.items():
        body = (CORPUS / "good" / source).read_bytes()
        assert r.api.call("POST", f"/api/v2/test/local?name={name}", body, timeout=30).status == 200
    uart.command("sched refresh 1")
    r.wait_player("the files are listed and one plays",
                  lambda s: s["channels"][0]["records"] >= 3 and s["playing"] and not s["loading"])
    yield r
    clean()
    uart.command("sched refresh 1")
    r.api.call("POST", "/api/v2/test/play/stop")


def test_a_tap_on_the_right_is_next_and_on_the_left_is_back(rig):
    start = rig.player()
    got = rig.fingers(tap(600, 300))
    assert (got["new"], got["last_gesture"], got["last_action"]) == (1, "tap", "next")
    after = rig.wait_player("next was shown", lambda s: s["counters"]["shown"] == start["counters"]["shown"] + 1
                            and not s["loading"])
    assert after["current"] != start["current"]
    got = rig.fingers(tap(100, 300))
    assert (got["last_gesture"], got["last_action"]) == ("tap", "back")
    back = rig.wait_player("back was shown", lambda s: s["history"]["position"] == 1 and not s["loading"])
    assert back["current"] == start["current"]


def test_a_touch_that_drifts_does_nothing_and_a_swipe_is_recognised(rig):
    shown = rig.player()["counters"]["shown"]
    assert rig.fingers(drag(360, 400, 360, 300))["new"] == 0, "100 pixels: not a tap, not a swipe"
    got = rig.fingers(drag(360, 500, 360, 250))
    assert (got["new"], got["last_gesture"], got["last_action"]) == (1, "swipe_up", "like")
    got = rig.fingers(drag(360, 200, 360, 450))
    assert (got["last_gesture"], got["last_action"]) == ("swipe_down", "unlike")
    time.sleep(0.5)
    assert rig.player()["counters"]["shown"] == shown, "none of that changed the artwork"


def test_a_long_press_opens_the_information_screen_and_another_closes_it(rig):
    got = rig.fingers(long_press())
    assert (got["new"], got["last_gesture"], got["last_action"]) == (1, "long_press", "open_info")
    ui = rig.api.get("/api/v2/ui").data
    assert ui["screen"] == "info" and ui["title"] == "p3a-fab"
    assert "http://p3a-fab.local" in ui["text"] and "Firmware 2.0.0" in ui["text"] and "free on the card" in ui["text"]
    # Under the screen a tap does nothing.
    shown = rig.player()["counters"]["shown"]
    assert rig.fingers(tap(600, 300))["last_action"] == "none"
    time.sleep(0.5)
    assert rig.player()["counters"]["shown"] == shown
    assert rig.fingers(long_press())["last_action"] == "close_screen"
    assert rig.screen() == "artwork"


def test_while_paused_only_a_long_press_works_and_it_resumes(rig):
    assert rig.api.call("POST", "/api/v2/player/pause").status == 200
    try:
        shown = rig.player()["counters"]["shown"]
        assert rig.fingers(tap(600, 300))["last_action"] == "none"
        assert rig.player()["counters"]["shown"] == shown and rig.player()["paused"]
        assert rig.fingers(long_press())["last_action"] == "resume"
        assert rig.player()["paused"] is False and rig.screen() == "artwork"
    finally:
        rig.uart.command("sched pause off")


def test_two_fingers_turn_the_screen_and_taps_follow_it(rig):
    # Two fingers 300 apart around the centre, turned by 60 degrees in steps.
    import math
    turn = []
    for degrees in range(0, 61, 6):
        a = math.radians(degrees)
        dx, dy = int(150 * math.cos(a)), int(150 * math.sin(a))
        turn.append([2, 360 + dx, 360 + dy, 360 - dx, 360 - dy, 30])
    turn.append(LIFT)
    got = rig.fingers(turn)
    assert (got["new"], got["last_gesture"], got["last_action"]) == (1, "rotate_cw", "turn_cw")
    try:
        assert rig.api.get("/api/v2/settings").data["rotation"] == 90, "and the setting is saved"
        # Turned 90 degrees clockwise, the viewer's right half is the bottom
        # half of the panel (as artworks are turned: test_player.TURN).
        assert rig.fingers(tap(360, 600))["last_action"] == "next"
        assert rig.fingers(tap(360, 100))["last_action"] == "back"
        # Back the other way.
        got = rig.fingers([[step[0], step[1], 720 - step[2], step[3], 720 - step[4], step[5]] for step in turn])
        assert got["last_action"] == "turn_ccw"
        assert rig.api.get("/api/v2/settings").data["rotation"] == 0
    finally:
        rig.uart.command("settings set", {"rotation": 0})


def test_an_update_on_the_screen_is_not_interrupted(rig):
    rig.uart.command("ui screen", {"screen": UPDATE, "up": True, "title": "Updating", "progress": 10})
    try:
        assert rig.fingers(tap(600, 300))["last_action"] == "none"
        assert rig.fingers(long_press())["last_action"] == "none"
        assert rig.screen() == "update"
    finally:
        rig.uart.command("ui screen", {"screen": UPDATE, "up": False})
