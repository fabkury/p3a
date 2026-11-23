# SPDX-License-Identifier: Apache-2.0
"""The animation player and the display pipeline under it: a file becomes
frames on the panel, through both upscalers, in every rotation, with
borders, blanking and source switching; and the player reports what became
of each request.

The device is given a file and asked what its panel is being shown; the test
builds the same frame on the workstation and compares. Pictures made here
are sent as BMP, which keeps every pixel.
"""
import io
import time
import zlib

import pytest
from PIL import Image, ImageDraw

import lab
from test_api import Api
from test_decoders import CORPUS, DECLARED_MS, reference_frames, shown_for

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

    def play(self, file: bytes, smooth: bool = False, wait: bool = True) -> dict:
        query = f"?smooth={int(smooth)}&wait={int(wait)}"
        reply = self.api.call("POST", "/api/v2/test/play" + query, file, timeout=90)
        assert reply.status == 200, reply.body[:200]
        return reply.data

    def show(self, img: Image.Image, mode: str = "cpu") -> dict:
        """Plays a picture made here and checks that the player showed it."""
        packed = io.BytesIO()
        img.save(packed, "BMP")
        data = self.play(packed.getvalue(), smooth=(mode == "ppa"))
        assert data["outcome"] == "shown", data
        assert (data["width"], data["height"], data["frame_count"]) == (img.width, img.height, 1)
        return data

    def state(self) -> dict:
        return self.api.get("/api/v2/test/play").data

    def screenshot(self, div: int = 3) -> Image.Image:
        reply = self.api.get(f"/api/v2/test/screenshot?div={div}", timeout=60)
        assert reply.status == 200
        return Image.open(io.BytesIO(reply.body)).convert("RGB")

    def differing(self, wanted: Image.Image, div: int = 3) -> int:
        """How many pixels of the panel differ from `wanted`. The device is asked
        for a checksum; the picture is downloaded only when that does not match."""
        b, g, r = wanted.split()[2], wanted.split()[1], wanted.split()[0]
        crc = zlib.crc32(Image.merge("RGB", (b, g, r)).tobytes())
        reply = self.api.get(f"/api/v2/test/screenshot?div={div}&crc=1", timeout=30)
        assert reply.status == 200 and (reply.data["w"], reply.data["h"]) == wanted.size
        if reply.data["crc"] == crc:
            return 0
        count = differing(self.screenshot(div), wanted)
        return count if count else -1  # the checksum and the picture disagree

    def set(self, **settings) -> None:
        assert self.api.call("PATCH", "/api/v2/settings", settings).status == 200

    def wait_draws(self, more_than: int, timeout: float = 5.0) -> dict:
        deadline = time.monotonic() + timeout
        state = self.state()
        while state["draws"] <= more_than and time.monotonic() < deadline:
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
    # Something for the scheduler to have: with an empty folder the UI says
    # "Nothing to play" on the panel whenever these tests stop the player.
    keep = "/api/v2/test/local?name=zz-player-keep.png"
    assert api.call("POST", keep, (CORPUS / "good" / "png" / "one_pixel.png").read_bytes(), timeout=30).status == 200
    uart.command("sched refresh 1")
    time.sleep(1.0)
    yield p
    api.call("POST", keep)
    uart.command("sched refresh 1")
    api.call("POST", "/api/v2/test/play/blank?on=0")
    api.call("POST", "/api/v2/test/play/stop")
    p.set(rotation=0, background_color="#000000", brightness=100)


def test_a_small_square_picture_fills_the_panel_exactly(panel, uart):
    img = picture(90, 90)
    panel.show(img)
    # Every one of the 518,400 pixels: each source pixel became an 8 by 8 block.
    assert panel.differing(expected_frame(img, 0), 1) == 0


@pytest.mark.parametrize("size", [(240, 180), (96, 160), (37, 53), (300, 20), (1, 1)])
def test_pictures_of_other_shapes_are_fitted_centred_and_bordered(panel, size):
    img = picture(*size)
    panel.show(img)
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0


def test_a_picture_larger_than_the_panel_is_scaled_down(panel):
    img = picture(900, 1200)
    panel.show(img)
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0


def test_rotation_follows_the_setting_without_a_new_picture(panel):
    img = picture(120, 80)  # not square, so a quarter turn changes the layout
    panel.show(img)
    try:
        for rotation in (90, 180, 270, 0):
            before = panel.state()["draws"]
            panel.set(rotation=rotation)
            state = panel.wait_draws(before)
            assert state["rotation"] == rotation
            assert state["draws"] == before + 1, "one redraw per change"
            assert panel.differing(sampled(expected_frame(img, rotation), 3), 3) == 0, rotation
    finally:
        panel.set(rotation=0)


def test_borders_follow_the_background_colour(panel):
    img = picture(200, 100)
    panel.show(img)
    try:
        for colour in ((32, 64, 96), (255, 255, 255)):
            before = panel.state()["draws"]
            panel.set(background_color="#%02x%02x%02x" % colour)
            panel.wait_draws(before)
            shot = panel.screenshot(div=3)
            assert shot.getpixel((5, 5)) == colour and shot.getpixel((120, 235)) == colour
            assert panel.differing(sampled(expected_frame(img, 0, colour), 3)) == 0
    finally:
        panel.set(background_color="#000000")


def test_a_picture_that_does_not_change_is_not_drawn_again(panel):
    img = picture(64, 64)
    panel.show(img)
    time.sleep(0.5)
    before = panel.state()["draws"]
    time.sleep(2.5)
    assert panel.state()["draws"] == before, "the producer idles on an unchanged picture"
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0, "and it is still there"


def test_blanking_shows_black_and_brings_the_picture_back(panel):
    img = picture(90, 90)
    panel.show(img)
    assert panel.api.call("POST", "/api/v2/test/play/blank?on=1").status == 200
    assert panel.screenshot(div=3).getcolors() == [(240 * 240, (0, 0, 0))]
    assert panel.api.call("POST", "/api/v2/test/play/blank?on=0").status == 200
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0


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
        before = panel.state()["draws"]
        uart.command("lcd brightness 100")
    panel.wait_draws(before)
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0


def test_switching_to_another_source_and_back(panel, uart):
    img = picture(80, 80)
    panel.show(img)
    uart.command("lcd bars")
    bars = panel.screenshot(div=3)
    assert bars.getpixel((5, 100)) == (255, 255, 255) and bars.getpixel((235, 100)) == (0, 0, 0)
    panel.show(img)
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0


@pytest.mark.parametrize("rotation", [0, 90])
def test_the_hardware_scaler_gives_a_smooth_picture_in_the_right_place(panel, rotation):
    img = picture(180, 180)
    try:
        panel.set(rotation=rotation)
        panel.show(img, mode="ppa")
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


def test_the_fps_overlay_appears_and_goes_with_its_setting(panel):
    black = Image.new("RGB", (8, 8))
    panel.show(black)
    all_black = [(240 * 240, (0, 0, 0))]
    assert panel.screenshot(div=3).getcolors() == all_black
    try:
        panel.set(show_fps=True)
        time.sleep(1.5)
        with_overlay = panel.screenshot(div=3)
        assert with_overlay.getcolors() != all_black, "the overlay is drawn even over a still"
        box = with_overlay.getbbox()
        assert (box[2] - box[0]) * (box[3] - box[1]) < 240 * 240 // 8, f"it is a small label, not {box}"
    finally:
        panel.set(show_fps=False)
    time.sleep(0.5)
    assert panel.screenshot(div=3).getcolors() == all_black, "and removed when switched off"


# --- files of every format, and what the player reports ---------------------------

def corpus(name: str) -> bytes:
    return (CORPUS / "good" / name).read_bytes()


def reports_for(data: dict) -> list[tuple]:
    return [(r["what"], r.get("error")) for r in data["reports"] if r["token"] == data["request"]]


@pytest.mark.parametrize("name", ["png/rgb.png", "gif/still.gif", "webp/lossless.webp", "bmp/rgb24.bmp",
                                  "png/apng_opaque.png", "gif/opaque_anim.gif", "webp/anim_opaque.webp"])
def test_a_file_of_each_format_reaches_the_panel(panel, name):
    frames = reference_frames(name, (0, 0, 0))
    first = Image.open(CORPUS / "good" / name)
    data = panel.play(corpus(name))
    assert data["outcome"] == "shown" and reports_for(data) == [("ready", None), ("shown", None)]
    assert (data["width"], data["height"], data["frame_count"]) == (*first.size, len(frames))
    if len(frames) == 1:
        wanted = expected_frame(Image.frombytes("RGB", first.size, frames[0]), 0)
        assert panel.differing(sampled(wanted, 3), 3) == 0


def test_a_jpeg_plays_whatever_its_file_is_called(panel):
    # Every upload is saved as ".bin": the format is read from the bytes.
    data = panel.play(corpus("jpeg/baseline.jpg"))
    assert data["outcome"] == "shown" and (data["width"], data["height"]) == (64, 48)
    shot = panel.screenshot(div=3)
    assert shot.getpixel((120, 5)) == (0, 0, 0), "a 4:3 picture has borders above and below"
    assert len(set(shot.crop((0, 40, 240, 200)).get_flattened_data())) > 50, "and is a picture in the middle"


@pytest.mark.parametrize("name", ["gif/opaque_anim.gif", "webp/anim_opaque.webp", "png/apng_opaque.png"])
def test_an_animation_keeps_its_pace_and_starts_over(panel, name):
    loop_ms = sum(shown_for(ms) for ms in DECLARED_MS[name])
    data = panel.play(corpus(name))
    assert data["outcome"] == "shown"
    a = panel.state()
    began = time.monotonic()
    time.sleep(4.0)
    b = panel.state()
    elapsed_ms = (time.monotonic() - began) * 1000
    frames = b["frames_shown"] - a["frames_shown"]
    wanted = elapsed_ms / loop_ms * len(DECLARED_MS[name])
    assert 0.85 * wanted <= frames <= 1.15 * wanted, f"{frames} frames in {elapsed_ms:.0f} ms, about {wanted:.0f} wanted"
    assert b["loops"] - a["loops"] >= int(elapsed_ms / loop_ms) - 1, "the end of the file starts the next loop"
    assert b["decode_failed"] is False


def test_a_transparent_still_is_composed_again_when_the_background_changes(panel):
    name = "png/rgba_soft_alpha.png"
    try:
        panel.set(background_color="#000000")
        assert panel.play(corpus(name))["outcome"] == "shown"
        for colour in ((0, 0, 0), (200, 30, 90)):
            before = panel.state()["draws"]
            panel.set(background_color="#%02x%02x%02x" % colour)
            if colour != (0, 0, 0):
                panel.wait_draws(before)
            frame = Image.frombytes("RGB", (40, 40), reference_frames(name, colour)[0])
            assert panel.differing(sampled(expected_frame(frame, 0, colour), 3), 3) == 0, colour
    finally:
        panel.set(background_color="#000000")


def test_a_file_that_cannot_be_played_leaves_the_last_one_on_the_panel(panel):
    img = picture(60, 60)
    panel.show(img)
    on_panel = panel.state()["token"]
    bad = CORPUS / "bad"
    cases = [(b"<html>this is not a picture</html>" * 20, "NOT_PLAYABLE"),
             ((bad / "png" / "png_truncated_half.png").read_bytes(), "CORRUPT"),
             ((bad / "gif" / "claims_65535x65535.gif").read_bytes(), "NOT_PLAYABLE")]
    for file, error in cases:
        data = panel.play(file)
        assert data["outcome"] == "failed" and data["error"] == error, data
        assert reports_for(data) == [("failed", error)]
        assert data["token"] == on_panel and data["loading"] is False
    reply = panel.api.call("POST", "/api/v2/test/play?path=%2Fsdcard%2Fno%2Fsuch%2Ffile.gif")
    assert reply.data["outcome"] == "failed" and reply.data["error"] == "MISSING"
    assert panel.differing(sampled(expected_frame(img, 0), 3), 3) == 0, "still showing the last good one"


def test_a_newer_request_replaces_one_that_is_still_loading(panel):
    slow = io.BytesIO()
    picture(700, 700).save(slow, "BMP")  # 1.5 MB: takes a moment to read back from the card
    first = panel.play(slow.getvalue(), wait=False)["request"]
    second = panel.play(corpus("png/rgb.png"))
    assert second["outcome"] == "shown" and second["token"] == second["request"]
    told = {r["token"]: (r["what"], r.get("error")) for r in second["reports"] if r["what"] != "ready"}
    assert told[first] in (("failed", "ESP_ERR_NOT_FINISHED"), ("shown", None))
    assert told[second["request"]] == ("shown", None)
    assert second["width"] == 48, "the newer one is on the panel either way"


def test_stop_blanks_the_panel_and_reports_like_any_request(panel):
    panel.show(picture(100, 100))
    reply = panel.api.call("POST", "/api/v2/test/play/stop")
    assert reply.data["outcome"] == "shown" and reply.data["frame_count"] == 0
    assert panel.screenshot(div=3).getcolors() == [(240 * 240, (0, 0, 0))]


@pytest.mark.slow
def test_playing_many_files_leaves_no_memory_behind(panel, uart):
    names = ["gif/opaque_anim.gif", "png/rgb.png", "webp/anim_opaque.webp", "jpeg/baseline.jpg", "png/apng_opaque.png"]
    bad = (CORPUS / "bad" / "gif" / "gif_truncated_half.gif").read_bytes()

    def a_round():
        for name in names:
            assert panel.play(corpus(name))["outcome"] == "shown"
        panel.play(bad)
        panel.api.call("POST", "/api/v2/test/play/stop")

    a_round()
    time.sleep(0.5)
    before = panel.state()
    for _ in range(4):
        a_round()
    time.sleep(0.5)
    after = panel.state()
    for pool in ("psram_free", "internal_free"):
        assert before[pool] - after[pool] < 32 * 1024, f"{pool} went from {before[pool]} to {after[pool]}"
    assert uart.command("mem")["level"] == "ok"
