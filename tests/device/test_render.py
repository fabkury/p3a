# SPDX-License-Identifier: Apache-2.0
"""The display pipeline: a picture from the artwork source to the panel's
framebuffer, through both upscalers, in every rotation, with borders,
blanking and source switching.

The device is given raw pixels and asked what its panel is being shown; the
test builds the same frame on the workstation and compares.
"""
import io
import time

import pytest
from PIL import Image, ImageDraw

import lab
from test_api import Api

pytestmark = pytest.mark.live

PANEL = 720
TURN = {0: None, 90: Image.Transpose.ROTATE_270, 180: Image.Transpose.ROTATE_180, 270: Image.Transpose.ROTATE_90}


def picture(w: int, h: int) -> Image.Image:
    """Different at every pixel, with marks that show which way is up."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // max(1, w - 1), (y * 255) // max(1, h - 1), (x * 31 + y * 17) & 0xFF)
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, max(1, w // 5), max(1, h // 5)), fill=(255, 0, 0))        # red: top left
    draw.rectangle((w - 1 - max(1, w // 8), 0, w - 1, max(1, h // 8)), fill=(0, 255, 0))  # green: top right
    return img


def nearest(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Nearest-neighbour scaling as the firmware does it: index i takes i * src // dst."""
    w, h = img.size
    sw, sh = size
    src = img.load()
    out = Image.new("RGB", size)
    dst = out.load()
    columns = [x * w // sw for x in range(sw)]
    for y in range(sh):
        sy = y * h // sh
        for x in range(sw):
            dst[x, y] = src[columns[x], sy]
    return out


def fit(w: int, h: int) -> tuple[int, int, int, int]:
    """Where a w x h picture goes on the panel: scaled size and offsets."""
    sw, sh = PANEL, h * PANEL // w
    if sh > PANEL:
        sw, sh = w * PANEL // h, PANEL
    sw, sh = max(1, sw), max(1, sh)
    return sw, sh, (PANEL - sw) // 2, (PANEL - sh) // 2


def expected_frame(img: Image.Image, rotation: int, background=(0, 0, 0), bilinear: bool = False) -> Image.Image:
    turned = img if TURN[rotation] is None else img.transpose(TURN[rotation])
    sw, sh, ox, oy = fit(*turned.size)
    scaled = turned.resize((sw, sh), Image.BILINEAR) if bilinear else nearest(turned, (sw, sh))
    frame = Image.new("RGB", (PANEL, PANEL), background)
    frame.paste(scaled, (ox, oy))
    return frame


class Panel:
    def __init__(self, api: Api):
        self.api = api

    def show(self, img: Image.Image, mode: str = "cpu") -> dict:
        reply = self.api.call("POST", f"/api/v2/test/render?w={img.width}&h={img.height}&mode={mode}",
                              img.tobytes(), timeout=60)
        assert reply.status == 200, reply.body[:200]
        return reply.data

    def state(self) -> dict:
        return self.api.get("/api/v2/test/render").data

    def screenshot(self, div: int = 3) -> Image.Image:
        reply = self.api.get(f"/api/v2/test/screenshot?div={div}", timeout=60)
        assert reply.status == 200
        return Image.open(io.BytesIO(reply.body)).convert("RGB")

    def set(self, **settings) -> None:
        assert self.api.call("PATCH", "/api/v2/settings", settings).status == 200

    def wait_frames(self, more_than: int, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        state = self.state()
        while state["frames_drawn"] <= more_than and time.monotonic() < deadline:
            time.sleep(0.1)
            state = self.state()
        time.sleep(0.15)  # the frame just drawn is on the panel a few refreshes later
        return state


def sampled(frame: Image.Image, div: int) -> Image.Image:
    """What a screenshot with this divisor keeps of a full frame: the middle pixel of each block."""
    if div == 1:
        return frame
    src = frame.load()
    out = Image.new("RGB", (PANEL // div, PANEL // div))
    dst = out.load()
    for y in range(PANEL // div):
        for x in range(PANEL // div):
            dst[x, y] = src[x * div + div // 2, y * div + div // 2]
    return out


def differing(a: Image.Image, b: Image.Image) -> int:
    assert a.size == b.size
    return sum(1 for p, q in zip(a.get_flattened_data(), b.get_flattened_data()) if p != q)


def mean_difference(a: Image.Image, b: Image.Image) -> float:
    total = sum(abs(x - y) for p, q in zip(a.get_flattened_data(), b.get_flattened_data()) for x, y in zip(p, q))
    return total / (a.width * a.height * 3)


@pytest.fixture(scope="module")
def panel(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    p = Panel(api)
    p.set(rotation=0, background_color="#000000", brightness=100, show_fps=False, playback_max_speed=False)
    yield p
    api.call("POST", "/api/v2/test/render/blank?on=0")
    api.call("POST", "/api/v2/test/render/release")
    p.set(rotation=0, background_color="#000000", brightness=100)


def test_a_small_square_picture_fills_the_panel_exactly(panel, uart):
    img = picture(90, 90)
    placed = panel.show(img)
    assert (placed["scaled_w"], placed["scaled_h"], placed["offset_x"], placed["offset_y"]) == (720, 720, 0, 0)
    assert placed["upscale"] == "ok"
    # Every one of the 518,400 pixels: each source pixel became an 8 by 8 block.
    assert differing(panel.screenshot(div=1), expected_frame(img, 0)) == 0


@pytest.mark.parametrize("size", [(240, 180), (96, 160), (100, 100), (37, 53), (300, 20), (1, 1), (720, 720)])
def test_pictures_of_other_shapes_are_fitted_centred_and_bordered(panel, size):
    img = picture(*size)
    placed = panel.show(img)
    sw, sh, ox, oy = fit(*size)
    assert (placed["scaled_w"], placed["scaled_h"], placed["offset_x"], placed["offset_y"]) == (sw, sh, ox, oy)
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0


def test_a_picture_larger_than_the_panel_is_scaled_down(panel):
    img = picture(900, 1200)
    placed = panel.show(img)
    assert (placed["scaled_w"], placed["scaled_h"]) == (540, 720)
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0


def test_rotation_follows_the_setting_without_a_new_picture(panel):
    img = picture(120, 80)  # not square, so a quarter turn changes the layout
    panel.show(img)
    try:
        for rotation in (90, 180, 270, 0):
            before = panel.state()["frames_drawn"]
            panel.set(rotation=rotation)
            state = panel.wait_frames(before)
            assert state["rotation"] == rotation
            assert state["frames_drawn"] == before + 1, "one redraw per change"
            sw, sh, ox, oy = fit(*(img.size if rotation in (0, 180) else img.size[::-1]))
            assert (state["scaled_w"], state["scaled_h"], state["offset_x"], state["offset_y"]) == (sw, sh, ox, oy)
            assert differing(panel.screenshot(div=3), sampled(expected_frame(img, rotation), 3)) == 0, rotation
    finally:
        panel.set(rotation=0)


def test_borders_follow_the_background_colour(panel):
    img = picture(200, 100)
    panel.show(img)
    try:
        for colour in ((32, 64, 96), (255, 255, 255)):
            before = panel.state()["frames_drawn"]
            panel.set(background_color="#%02x%02x%02x" % colour)
            panel.wait_frames(before)
            shot = panel.screenshot(div=3)
            assert shot.getpixel((5, 5)) == colour and shot.getpixel((120, 235)) == colour
            assert differing(shot, sampled(expected_frame(img, 0, colour), 3)) == 0
    finally:
        panel.set(background_color="#000000")


def test_a_picture_that_does_not_change_is_not_drawn_again(panel):
    img = picture(64, 64)
    panel.show(img)
    time.sleep(0.5)
    before = panel.state()["frames_drawn"]
    time.sleep(2.5)
    assert panel.state()["frames_drawn"] == before, "the producer idles on an unchanged picture"
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0, "and it is still there"


def test_blanking_shows_black_and_brings_the_picture_back(panel):
    img = picture(90, 90)
    panel.show(img)
    assert panel.api.call("POST", "/api/v2/test/render/blank?on=1").status == 200
    assert panel.screenshot(div=3).getcolors() == [(240 * 240, (0, 0, 0))]
    assert panel.api.call("POST", "/api/v2/test/render/blank?on=0").status == 200
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0


def test_brightness_zero_also_blanks(panel, uart):
    # The backlight is driven through the board here: the brightness setting
    # is applied at boot, and at run time by the screen's owner (M3.1).
    img = picture(90, 90)
    panel.show(img)
    try:
        uart.command("lcd brightness 0")
        time.sleep(0.4)
        assert panel.screenshot(div=3).getcolors() == [(240 * 240, (0, 0, 0))], "no point drawing in the dark"
    finally:
        before = panel.state()["frames_drawn"]
        uart.command("lcd brightness 100")
    panel.wait_frames(before)
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0


def test_switching_to_another_source_and_back(panel, uart):
    img = picture(80, 80)
    panel.show(img)
    uart.command("lcd bars")
    bars = panel.screenshot(div=3)
    assert bars.getpixel((5, 100)) == (255, 255, 255) and bars.getpixel((235, 100)) == (0, 0, 0)
    panel.show(img)
    assert differing(panel.screenshot(div=3), sampled(expected_frame(img, 0), 3)) == 0


@pytest.mark.parametrize("rotation", [0, 90])
def test_the_hardware_scaler_gives_a_smooth_picture_in_the_right_place(panel, rotation):
    img = picture(180, 180)
    try:
        panel.set(rotation=rotation)
        placed = panel.show(img, mode="ppa")
        assert placed["upscale"] == "ok"
        shot = panel.screenshot(div=3)
        wanted = sampled(expected_frame(img, rotation, bilinear=True), 3)
        mean = mean_difference(shot, wanted)
        assert mean < 6.0, f"mean difference {mean:.2f} from a bilinear reference"
        # Smooth, not blocky: neighbouring samples inside a gradient differ.
        row = [shot.getpixel((x, 150)) for x in range(100, 140)]
        assert len(set(row)) > 30
    finally:
        panel.set(rotation=0)


def test_the_hardware_scaler_fills_borders_with_the_background(panel):
    img = picture(240, 120)
    try:
        panel.set(background_color="#204060")
        panel.show(img, mode="ppa")
        shot = panel.screenshot(div=3)
        assert shot.getpixel((120, 5)) == (0x20, 0x40, 0x60) and shot.getpixel((120, 234)) == (0x20, 0x40, 0x60)
        assert shot.getpixel((120, 120)) != (0x20, 0x40, 0x60)
    finally:
        panel.set(background_color="#000000")


def test_bad_requests_are_refused(panel):
    api = panel.api
    api.call("POST", "/api/v2/test/render?w=10&h=10", b"x" * 299).error(400, "INVALID")
    api.call("POST", "/api/v2/test/render?w=0&h=10", b"").error(400, "INVALID")
    api.call("POST", "/api/v2/test/render?w=3000&h=1", b"x" * 9000).error(400, "INVALID")
