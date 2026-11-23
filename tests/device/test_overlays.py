# SPDX-License-Identifier: Apache-2.0
"""The overlays over the artwork: the clock, the corner icons and the
processing indicator, each compared pixel for pixel with the same drawing
code run on the workstation (build-host/ui_preview).

Their logic and drawing are host-tested (ui_clock, ui_image, ui_player,
ui_draw). This is the device's side: the settings reaching the overlay, the
clock following the time and the zone, an icon appearing over a still
artwork and going by itself.
"""
import io
import subprocess
import time

import pytest
from PIL import Image

import lab
from test_api import Api
from test_player import PANEL, Panel, expected_frame
from test_ui_drawing import FONT, PREVIEW, REPO

pytestmark = pytest.mark.live

IMAGES = REPO / "components" / "p3a_ui" / "images"
ART = Image.new("RGB", (80, 80), (30, 90, 160))
CLOCK_OFF = {"clock_enabled": False, "clock_24h": True, "clock_position": "top-right", "clock_size": 3,
             "clock_color": "#FFFFFF", "clock_outline_color": "#000000", "clock_outline": 3, "timezone": "UTC0"}


def overlay(tmp_path, under: Image.Image, mode: str, *args) -> Image.Image:
    out, base = tmp_path / "expected.bgr", tmp_path / "under.bgr"
    base.write_bytes(under.tobytes("raw", "BGR"))
    subprocess.run([str(PREVIEW), str(FONT), "0", mode, str(out), *[str(a) for a in args], str(base)], check=True)
    return Image.frombytes("RGB", (PANEL, PANEL), out.read_bytes(), "raw", "BGR")


@pytest.fixture(scope="module")
def rig(bring_up):
    assert PREVIEW.exists(), f"build the host tests first: {PREVIEW} is missing"
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    panel = Panel(api)
    panel.set(rotation=0, background_color="#000000", brightness=100, show_fps=False, **CLOCK_OFF)
    yield uart, api, panel
    panel.set(**CLOCK_OFF, indicator_enabled=True, indicator_size=64)
    api.call("POST", "/api/v2/test/play/stop")


def set_clock(api, unix: int) -> None:
    assert api.call("POST", "/api/v2/test/clock", {"unix": unix}).status == 200


def test_the_clock_shows_the_local_time_where_and_how_the_settings_say(rig, tmp_path):
    uart, api, panel = rig
    panel.show(ART)
    plain = expected_frame(ART, 0)
    # 2026-11-23 14:05:20 UTC.
    set_clock(api, 1795529120)
    panel.set(clock_enabled=True)
    time.sleep(0.6)
    assert panel.differing(overlay(tmp_path, plain, "clock", 2, 3, "FFFFFF", "000000", 3, "14:05"), 1) == 0

    # Another zone, 12-hour, another corner, size and colours.
    panel.set(timezone="EST5EDT,M3.2.0,M11.1.0", clock_24h=False, clock_position="bottom-left", clock_size=5,
              clock_color="#FFD040", clock_outline_color="#402000", clock_outline=5)
    time.sleep(0.6)
    assert panel.differing(overlay(tmp_path, plain, "clock", 6, 5, "FFD040", "402000", 5, "9:05"), 1) == 0

    # The minute turns on a still artwork: the clock is redrawn by itself.
    set_clock(api, 1795529120 + 58)  # 14:06:18 UTC
    time.sleep(1.0)
    assert panel.differing(overlay(tmp_path, plain, "clock", 6, 5, "FFD040", "402000", 5, "9:06"), 1) == 0

    panel.set(clock_enabled=False)
    time.sleep(0.6)
    assert panel.differing(plain, 1) == 0, "and off again leaves the artwork as it was"


def test_a_swipe_is_answered_with_an_icon_that_goes_by_itself(rig, tmp_path):
    uart, api, panel = rig
    panel.show(ART)
    plain = expected_frame(ART, 0)
    assert panel.differing(plain, 1) == 0
    swipe = [[1, 360, 500 - 30 * i, 0, 0, 30] for i in range(9)] + [[0, 0, 0, 0, 0, 60]]
    assert uart.command("ui touch", swipe) == {"result": "ok"}
    time.sleep(1.2)
    assert uart.command("ui touched")["last_gesture"] == "swipe_up"
    # A file of the user's own cannot be liked: the error icon, top right.
    assert panel.differing(overlay(tmp_path, plain, "icon", 0, IMAGES / "error.p3aimg"), 1) == 0
    time.sleep(4.0)
    assert panel.differing(plain, 1) == 0, "gone after four seconds, from a still artwork too"


def shot(api) -> Image.Image:
    return Image.open(io.BytesIO(api.get("/api/v2/test/screenshot", timeout=60).body)).convert("RGB")


def step(api, verb: str) -> dict:
    before = api.get("/api/v2/player").data["counters"]
    assert api.call("POST", f"/api/v2/player/{verb}").status == 200
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        state = api.get("/api/v2/player").data
        now = state["counters"]
        if now["shown"] + now["failed"] > before["shown"] + before["failed"] and not state["loading"]:
            return state
        time.sleep(0.2)
    raise AssertionError(f"{verb} was never reported")


def test_the_indicator_flashes_red_when_a_request_cannot_be_shown(rig):
    uart, api, panel = rig
    from test_decoders import CORPUS
    files = {"zz-overlay-a.png": "png/rgb.png", "zz-overlay-b.gif": "gif/opaque_anim.gif"}
    for name, source in files.items():
        body = (CORPUS / "good" / source).read_bytes()
        assert api.call("POST", f"/api/v2/test/local?name={name}", body, timeout=30).status == 200
    uart.command("sched activate", {"name": "", "channels": [{"source": "local"}]})
    uart.command("sched refresh 1")
    time.sleep(2.0)
    try:
        first = step(api, "next")
        second = step(api, "next")
        assert first["current"] != second["current"]
        api.call("POST", f"/api/v2/test/local?name={first['current']['name']}")  # gone behind the scheduler's back
        failed = step(api, "back")
        assert failed["counters"]["failed"] == second["counters"]["failed"] + 1
        time.sleep(0.4)
        red = (0xFF, 0x30, 0x30)
        seen = shot(api)
        assert seen.getpixel((719, 719)) == red and seen.getpixel((717, 719)) == red, "the indicator's corner is red"
        assert seen.getpixel((718, 719)) != red, "and checkered"
        time.sleep(3.5)
        assert shot(api).getpixel((719, 719)) != red, "three seconds later it is gone"
    finally:
        for name in files:
            api.call("POST", f"/api/v2/test/local?name={name}")
        uart.command("sched refresh 1")
