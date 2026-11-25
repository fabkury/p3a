# SPDX-License-Identifier: Apache-2.0
"""The test hooks reached over HTTP: screenshot, log capture, clock."""
import io
import time

import pytest
from PIL import Image

import lab


@pytest.fixture(scope="module", autouse=True)
def panel_handed_back(bring_up):
    """A test pattern holds the panel until it is taken down. Left up, the player's artworks are
    not drawn and every later test that waits for one to be shown hangs."""
    yield
    bring_up[0].command("lcd done")
from test_api import Api

pytestmark = pytest.mark.live


@pytest.fixture
def api(uart):
    return Api(lab.provision(uart)["ip"])


def test_a_screenshot_over_http_matches_the_one_over_uart(api, uart, tmp_path):
    uart.command("lcd bars")
    slow = Image.open(uart.screenshot(str(tmp_path / "uart.png"), 8)).convert("RGB")
    reply = api.get("/api/v2/test/screenshot?div=8")
    assert reply.status == 200 and reply.headers["content-type"] == "image/bmp"
    assert reply.headers["cache-control"] == "no-store"
    fast = Image.open(io.BytesIO(reply.body)).convert("RGB")
    assert fast.size == slow.size == (90, 90)
    assert fast.tobytes() == slow.tobytes(), "the same pixels by both routes"


def test_a_full_screenshot_is_quick_and_exact(api, uart):
    uart.command("lcd fill 200 100 50")
    began = time.monotonic()
    reply = api.get("/api/v2/test/screenshot", timeout=30)
    took = time.monotonic() - began
    image = Image.open(io.BytesIO(reply.body)).convert("RGB")
    assert image.size == (720, 720)
    assert len(reply.body) == 54 + 720 * 720 * 3
    assert image.getcolors() == [(720 * 720, (200, 100, 50))], "every pixel is the colour that was filled"
    assert took < 20, f"1.5 MB took {took:.1f} s (about 8 s is normal: the link sends roughly 190 KB/s)"
    # A pattern with a known pixel, to pin down orientation and channel order.
    uart.command("lcd gradient")
    image = Image.open(io.BytesIO(api.get("/api/v2/test/screenshot", timeout=30).body)).convert("RGB")
    for x, y in ((0, 0), (719, 0), (0, 719), (360, 200), (719, 719)):
        expected = uart.command(f"lcd pixel {x} {y}")
        assert image.getpixel((x, y)) == (expected["r"], expected["g"], expected["b"]), (x, y)


def test_screenshot_arguments_are_checked(api, uart):
    uart.command("lcd bars")
    api.get("/api/v2/test/screenshot?div=7").error(400, "INVALID")
    api.get("/api/v2/test/screenshot?div=0").error(400, "INVALID")
    api.get("/api/v2/test/screenshot?div=abc").error(400, "INVALID")
    assert api.get("/api/v2/test/screenshot?div=90").status == 200


def test_log_lines_are_captured_and_paged(api, uart):
    first = api.get("/api/v2/test/log").data
    assert first["next"] >= len(first["lines"]) > 0, "the boot log is there"
    assert first["first"] == first["next"] - len(first["lines"]) + 1
    # Something that logs a line we can recognise.
    uart.command('settings set {"show_fps":true}')
    uart.command('settings set {"show_fps":false}')
    more = api.get(f"/api/v2/test/log?since={first['next']}").data
    assert sum("settings: show_fps changed" in line for line in more["lines"]) == 2
    assert more["next"] >= first["next"] + 2
    # Nothing new: an empty page, same position.
    again = api.get(f"/api/v2/test/log?since={more['next']}").data
    assert again["lines"] == [] or all("show_fps" not in line for line in again["lines"])
    # A position from the future is simply empty.
    assert api.get(f"/api/v2/test/log?since={more['next'] + 100000}").data["lines"] == []


@pytest.mark.slow
def test_the_log_keeps_only_the_most_recent_lines(api, uart):
    # Each change logs one line; enough of them to wrap the buffer whatever was in it.
    for i in range(270):
        uart.command('settings set {"show_fps":%s}' % ("true" if i % 2 == 0 else "false"))
    page = api.get("/api/v2/test/log").data
    assert len(page["lines"]) == 256, "the buffer holds 256 lines"
    assert page["first"] == page["next"] - 255
    uart.command('settings set {"show_fps":false}')


def test_the_clock_can_be_set_for_a_test(api, uart):
    real = time.time()
    try:
        reply = api.call("POST", "/api/v2/test/clock", {"unix": 1893456000})  # 2030-01-01T00:00:00Z
        assert reply.data["set"] is True and reply.data["now"].startswith("2030-01-01T00:00:0")
        assert api.get("/api/v2/status").data["time"]["now"].startswith("2030-01-01T00:00:")
        assert uart.command("clock")["now"].startswith("2030-01-01")
        api.call("POST", "/api/v2/test/clock", {"unix": "soon"}).error(400, "INVALID")
        api.call("POST", "/api/v2/test/clock", {}).error(400, "INVALID")
        api.call("POST", "/api/v2/test/clock", {"unix": -5}).error(400, "INVALID")
    finally:
        api.call("POST", "/api/v2/test/clock", {"unix": int(time.time())})
    assert abs(api.get("/api/v2/test/clock").data["unix"] - time.time()) < 5
    assert time.time() - real < 60
