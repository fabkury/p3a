# SPDX-License-Identifier: Apache-2.0
"""The panel's framebuffers, the backlight and the BOOT button.

These tests read back what was written to the framebuffers. They cannot see
the glass: whether the panel really shows the pattern is a manual check.
"""
import zlib

import pytest

import lab


@pytest.fixture(scope="module", autouse=True)
def panel_handed_back(bring_up):
    """A test pattern holds the panel until it is taken down. Left up, the player's artworks are
    not drawn and every later test that waits for one to be shown hangs."""
    yield
    bring_up[0].command("lcd done")

W = H = 720


def test_panel_and_framebuffers_are_up(uart):
    info = uart.command("lcd info")
    assert info["panel"] is True
    assert (info["width"], info["height"]) == (W, H)
    assert info["buffers"] == 3, "the display pipeline needs three framebuffers"
    assert info["stride"] >= W * 3
    assert info["buffer_bytes"] >= info["stride"] * H
    assert info["brightness"] == 100, "the backlight comes up at full brightness"


def test_boot_log_shows_the_display_coming_up(uart):
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start)
    boot = uart.lines[start:]
    assert any("Display initialized: 3 buffers" in line for line in boot)
    assert not any("boot: display failed" in line for line in boot)


def test_solid_fill_reads_back_everywhere(uart):
    uart.command("lcd fill 10 200 30")
    for x, y in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1), (W // 2, H // 2), (123, 456)]:
        assert uart.command(f"lcd pixel {x} {y}") == {"r": 10, "g": 200, "b": 30}, (x, y)
    # The whole visible frame, not only the probes: 720 rows of the same pixel.
    expected = zlib.crc32(bytes([30, 200, 10]) * W * H)  # stored blue, green, red
    assert uart.command("lcd crc")["crc"] == expected


def test_a_new_frame_is_never_drawn_into_the_one_on_screen(uart):
    # The pipeline hands the source a free framebuffer; the one being scanned
    # out is not free. So consecutive frames land in different buffers.
    seen = [uart.command("lcd fill 1 2 3")["buffer"] for _ in range(8)]
    assert all(0 <= index <= 2 for index in seen)
    assert all(a != b for a, b in zip(seen, seen[1:])), seen
    assert len(set(seen)) >= 2


def test_colour_bars_have_the_right_colours_in_the_right_places(uart):
    uart.command("lcd bars")
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    for i, (r, g, b) in enumerate(bars):
        for x in (i * 90, i * 90 + 45, i * 90 + 89):  # both edges and the middle of each bar
            for y in (0, 359, 719):
                assert uart.command(f"lcd pixel {x} {y}") == {"r": r, "g": g, "b": b}, (i, x, y)


def test_gradient_addresses_every_row_and_column(uart):
    uart.command("lcd gradient")
    assert uart.command("lcd pixel 0 0") == {"r": 255, "g": 255, "b": 255}      # border
    assert uart.command("lcd pixel 719 719") == {"r": 255, "g": 255, "b": 255}
    assert uart.command("lcd pixel 30 30") == {"r": 255, "g": 0, "b": 0}        # corner block
    for x, y in [(100, 100), (360, 200), (700, 60), (61, 700), (718, 718)]:
        expected = {"r": x * 255 // 719, "g": y * 255 // 719, "b": (x + y) & 0xFF}
        assert uart.command(f"lcd pixel {x} {y}") == expected, (x, y)


def test_screenshot_matches_what_was_drawn(uart, tmp_path):
    from PIL import Image

    uart.command("lcd bars")
    path = uart.screenshot(tmp_path / "bars.png", divisor=8)
    image = Image.open(path).convert("RGB")
    assert image.size == (90, 90)
    # Sampled at the centre of each 8x8 block; bars are 90 px wide, so 11.25 samples each.
    assert image.getpixel((2, 45)) == (255, 255, 255)
    assert image.getpixel((15, 45)) == (255, 255, 0)
    assert image.getpixel((60, 10)) == (255, 0, 0)
    assert image.getpixel((88, 80)) == (0, 0, 0)


def test_brightness_is_clamped_and_remembered(uart):
    try:
        assert uart.command("lcd brightness 40")["brightness"] == 40
        assert uart.command("lcd info")["brightness"] == 40
        assert uart.command("lcd brightness 250")["brightness"] == 100
        assert uart.command("lcd brightness -5")["brightness"] == 0
    finally:
        uart.command("lcd brightness 100")


def test_button_watch_starts_and_nobody_pressed_it(uart):
    assert uart.command("button")["presses"] == 0
    assert uart.command("button")["presses"] == 0, "asking twice must not re-initialize"


def test_bad_lcd_arguments(uart):
    for bad in ("lcd", "lcd pixel 720 0", "lcd pixel 0 -1", "lcd shot 7", "lcd fill 1 2", "lcd nonsense"):
        with pytest.raises(lab.LabError):
            uart.command(bad)


def test_touch_controller_answers(uart):
    # Nobody is touching the panel during an automated run.
    assert uart.command("touch") == {"ready": True, "points": 0, "x": 0, "y": 0}
    for _ in range(20):
        assert uart.command("touch")["ready"] is True


def test_touch_comes_up_at_boot_without_delay(uart):
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start)
    boot = uart.lines[start:]
    assert not any("boot: touch failed" in line for line in boot)
    assert not any("touch controller init" in line for line in boot), "no failure or timeout message"
    assert uart.command("touch")["ready"] is True
