# SPDX-License-Identifier: Apache-2.0
"""The UI on the panel: a screen takes the whole panel, a notice is a band
over the playing artwork, and both are exactly what the drawing code makes.

The layout and the text renderer are host-tested (test_ui_text,
test_ui_draw). Here the panel is compared, pixel for pixel, with the same
code run on the workstation (build-host/ui_preview): what this adds is the
device's side of it: the font compiled in, the framebuffer's layout, the
rotation, the switch between the artwork and the UI, and redrawing when
the words change.

Needs the host build (cmake --build build-host), which a checkpoint makes
before the device tests anyway.
"""
import subprocess
import sys
import time
from pathlib import Path

import pytest
from PIL import Image

import lab
from test_api import Api
from test_player import PANEL, Panel, expected_frame

pytestmark = pytest.mark.live

REPO = Path(__file__).resolve().parents[2]
PREVIEW = REPO / "build-host" / ("ui_preview.exe" if sys.platform == "win32" else "ui_preview")
FONT = REPO / "components" / "p3a_ui" / "fonts" / "ui.p3afont"
UPDATE = 8         # p3a_screen_t
CARD_REPAIRED = 5  # p3a_notice_t


def preview(tmp_path, rotation: int, mode: str, progress: int, title: str, text: str,
            under: Image.Image | None = None, hint: str | None = None) -> Image.Image:
    """The frame the drawing code makes for these words, from the workstation."""
    out = tmp_path / "expected.bgr"
    args = [str(PREVIEW), str(FONT), str(rotation), mode, str(out), str(progress), title, text]
    if under is not None:
        base = tmp_path / "under.bgr"
        base.write_bytes(under.tobytes("raw", "BGR"))
        args.append(str(base))
    elif hint is not None:
        args.append(hint)
    subprocess.run(args, check=True)
    return Image.frombytes("RGB", (PANEL, PANEL), out.read_bytes(), "raw", "BGR")


def settle() -> None:
    time.sleep(0.5)  # the model's change, the source switch, and a few refreshes


@pytest.fixture(scope="module")
def rig(bring_up):
    assert PREVIEW.exists(), f"build the host tests first: {PREVIEW} is missing"
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    panel = Panel(api)

    def clean():
        uart.command("ui screen", {"screen": UPDATE, "up": False})
        uart.command("ui notice", {"notice": CARD_REPAIRED, "up": False})
        panel.set(rotation=0, background_color="#000000", brightness=100, show_fps=False)

    clean()
    yield uart, api, panel
    clean()
    api.call("POST", "/api/v2/test/play/stop")


def test_a_screen_takes_the_panel_and_gives_it_back(rig, tmp_path):
    uart, api, panel = rig
    art = Image.new("RGB", (90, 90), (20, 140, 220))
    panel.show(art)
    words = {"title": "Updating Wi-Fi", "text": "2.9.1 to 2.9.3. Do not unplug", "progress": 40}
    uart.command("ui screen", {"screen": UPDATE, "up": True, **words})
    settle()
    assert panel.differing(preview(tmp_path, 0, "screen", 40, words["title"], words["text"]), 1) == 0

    # New words are drawn; the same words are not drawn again.
    uart.command("ui screen", {"screen": UPDATE, "up": True, **words, "progress": 41})
    settle()
    assert panel.differing(preview(tmp_path, 0, "screen", 41, words["title"], words["text"]), 1) == 0

    uart.command("ui screen", {"screen": UPDATE, "up": False})
    settle()
    assert panel.differing(expected_frame(art, 0), 1) == 0, "the artwork is back, untouched"


@pytest.mark.parametrize("rotation", [90, 180, 270])
def test_a_screen_is_upright_however_the_device_is_turned(rig, tmp_path, rotation):
    uart, api, panel = rig
    panel.set(rotation=rotation)
    words = {"title": "Wi-Fi setup", "text": "Join the Wi-Fi network p3a-setup with a phone or a computer"}
    uart.command("ui screen", {"screen": UPDATE, "up": True, **words})
    try:
        settle()
        assert panel.differing(preview(tmp_path, rotation, "screen", -1, words["title"], words["text"]), 1) == 0
    finally:
        uart.command("ui screen", {"screen": UPDATE, "up": False})
        panel.set(rotation=0)


def test_rows_and_a_hint_are_drawn_as_on_the_workstation(rig, tmp_path):
    uart, api, panel = rig
    text = "Join this network with a phone.\n\nNetwork\tp3a-setup\nOr open\thttp://192.168.4.1"
    uart.command("ui screen", {"screen": UPDATE, "up": True, "title": "Wi-Fi setup", "text": text,
                               "hint": "Long press to hide"})
    try:
        settle()
        wanted = preview(tmp_path, 0, "screen", -1, "Wi-Fi setup", text, hint="Long press to hide")
        assert panel.differing(wanted, 1) == 0
    finally:
        uart.command("ui screen", {"screen": UPDATE, "up": False})


def test_a_notice_is_a_band_over_a_still_artwork_and_leaves_no_trace(rig, tmp_path):
    uart, api, panel = rig
    art = Image.new("RGB", (60, 60), (200, 60, 90))
    art.putpixel((0, 59), (255, 255, 255))  # something in the corner the band covers
    panel.show(art)
    plain = expected_frame(art, 0)
    assert panel.differing(plain, 1) == 0
    text = "The SD card was repaired"
    uart.command("ui notice", {"notice": CARD_REPAIRED, "up": True, "title": "SD card", "text": text})
    settle()
    assert api.get("/api/v2/ui").data["banner"] is True
    # A still is not redrawn by itself: the band appearing proves the redraw was asked for.
    assert panel.differing(preview(tmp_path, 0, "banner", -1, "SD card", text, under=plain), 1) == 0
    uart.command("ui notice", {"notice": CARD_REPAIRED, "up": False})
    settle()
    assert panel.differing(plain, 1) == 0
