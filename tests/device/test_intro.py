# SPDX-License-Identifier: Apache-2.0
"""The boot animation on the device: it is the first thing on the panel, it
ends on the logo, and it gives the panel to the artwork that loaded under
it.

Each animation's own contract (a plain background at the start, the logo
at the end, nothing written outside the frame) is checked on the
workstation by host/intro-anim-lab. This is the device's side: the choice
from the settings, the clock, and the hand-over.
"""
import io
import time

import pytest
from PIL import Image

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = [pytest.mark.live, pytest.mark.slow]

LOGO_W, LOGO_H, SCALE = 46, 54, 3
KEEP = "zz-intro-a.png"


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    body = (CORPUS / "good" / "png" / "rgb.png").read_bytes()
    assert api.call("POST", f"/api/v2/test/local?name={KEEP}", body, timeout=30).status == 200
    uart.command("sched activate", {"name": "", "channels": [{"source": "local"}]})
    yield uart, api
    uart.command("settings set", {"intro_animation": "", "intro_duration_ms": 3000, "background_color": "#000000"})
    api.call("POST", f"/api/v2/test/local?name={KEEP}")
    uart.command("sched refresh 1")


def reboot(uart) -> float:
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    return time.monotonic()


def test_the_chosen_animation_plays_ends_on_the_logo_and_hands_over(rig):
    uart, api = rig
    uart.command("settings set", {"intro_animation": "smoothstep-fade", "intro_duration_ms": 1000,
                                  "background_color": "#102040"})
    since = uart.mark()
    reboot(uart)
    uart.wait_for(r"boot animation 'smoothstep-fade' \(set\), 1000 ms", timeout=5, since=since)

    # The network comes back while the logo is still held or just after. The
    # model says which; both are checked when seen.
    api = Api(lab.provision(uart)["ip"])
    ui = api.get("/api/v2/ui").data
    if ui["screen"] == "intro":
        shot = Image.open(io.BytesIO(api.get("/api/v2/test/screenshot", timeout=60).body)).convert("RGB")
        left, top = (720 - LOGO_W * SCALE) // 2, (720 - LOGO_H * SCALE) // 2
        assert shot.getpixel((5, 5)) == (0x10, 0x20, 0x40), "the background colour around the logo"
        assert shot.getpixel((left - 4, 360)) == (0x10, 0x20, 0x40) and shot.getpixel((360, top - 4)) == (0x10, 0x20, 0x40)
        inside = shot.crop((left, top, left + LOGO_W * SCALE, top + LOGO_H * SCALE))
        colours = {c for _n, c in inside.getcolors(maxcolors=100000)}
        assert len(colours) > 1, "something is drawn where the logo goes"

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and api.get("/api/v2/ui").data["screen"] == "intro":
        time.sleep(0.2)
    ui = api.get("/api/v2/ui").data
    assert ui["screen"] == "artwork", ui
    player = api.get("/api/v2/player").data
    assert player["playing"] and player["current"]["source"] == "local", "the artwork that loaded under it is showing"
    done = uart.wait_for(r"boot animation 'smoothstep-fade': (\d+) frames.* (\d+) over the 33 ms budget", timeout=5,
                         since=since)
    assert int(done.group(1)) > 30 and int(done.group(2)) == 0, "it ran its length and every frame was on time"


def test_an_unknown_name_falls_back_to_a_random_one(rig):
    uart, api = rig
    uart.command("settings set", {"intro_animation": "no-such-animation", "intro_duration_ms": 1000})
    since = uart.mark()
    reboot(uart)
    uart.wait_for(r"no boot animation is called 'no-such-animation'", timeout=5, since=since)
    uart.wait_for(r"\(random\), 1000 ms", timeout=5, since=since)
