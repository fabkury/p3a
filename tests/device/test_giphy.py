# SPDX-License-Identifier: Apache-2.0
"""The Giphy source on the device, against Giphy itself: a channel without a
key, with a key Giphy refuses, and with the test rig's key (from the secrets
file): listed, downloaded, played.

Reading the listing is host-tested on recorded responses
(test_giphy_parse.c); this is the wiring and the live service. It costs
about five requests of the key's hourly allowance per run.
"""
import time

import pytest

import lab
from test_api import Api
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live

# An offset of its own for every run makes channels the device has not seen: one it has seen has
# its index on the card and is not listed again until that is two hours old.
RUN = int(time.time()) % 300


def playset(kind: str, identifier: str = "", offset: int = RUN) -> dict:
    return {"name": "Giphy test", "channels": [{"source": "giphy", "kind": kind, "identifier": identifier,
                                               "offset": offset}]}


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    r.key = lab.load_secrets()["api_keys"]["giphy_api_key"]
    # Small files and one page of listing: the test is of the wiring, not of Giphy's catalogue.
    uart.command("settings set", {"giphy_api_key": "", "giphy_cache_size": 32, "giphy_prefer_downsized": False,
                                  "giphy_rating": "g", "dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"giphy_api_key": "", "giphy_cache_size": 350, "giphy_prefer_downsized": True,
                                  "giphy_rating": "pg-13", "dwell_s": 30})
    uart.command("library evict 1000000000000 0 -", timeout=120)  # the GIFs fetched here


def channel(status):
    return status["channels"][0]


def test_without_a_key_the_channel_says_so_and_nothing_is_asked_of_giphy(rig):
    assert rig.uart.command("sched activate", playset("trending")) == {"result": "ok"}
    status = rig.wait("the listing answered", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] != "listing")
    ch = channel(status)
    assert ch["source"] == "giphy" and ch["state"] == "failed" and ch["error"] == "NO_KEY"
    assert 3500 < ch["refresh_in_s"] <= 3600, "asked again in an hour, or when a setting changes"
    # Said on the screen: as the whole notice when nothing plays, over the artwork when something
    # of an earlier playset is still up.
    rig.wait("the screen says why", lambda s: "key" in rig.api.get("/api/v2/ui").data.get("text", ""), timeout=10)
    assert rig.api.get("/api/v2/ui").data["text"] in ("A source needs its key. Enter it on the web page",
                                                      "Giphy needs an API key. Enter it on the web page")


def test_a_key_giphy_refuses_is_reported_as_such(rig):
    before = channel(rig.status())["listings"]
    # Saving a key is a change of settings: the channel is listed again by itself.
    rig.uart.command("settings set", {"giphy_api_key": "0000000000000000"})
    status = rig.wait("giphy answered", lambda s: channel(s)["listings"] > before and channel(s)["state"] != "listing",
                      timeout=60)
    assert channel(status)["error"] == "AUTH" and channel(status)["state"] == "failed"
    assert 3500 < channel(status)["refresh_in_s"] <= 3600


def test_with_a_key_trending_is_listed_downloaded_and_played(rig):
    before = rig.status()
    rig.uart.command("settings set", {"giphy_api_key": rig.key})
    status = rig.wait("listed", lambda s: channel(s)["listings"] > channel(before)["listings"]
                      and channel(s)["state"] == "ready", timeout=60)
    ch = channel(status)
    assert "error" not in ch and 16 <= ch["records"] <= 32, "cut to the 32 GIFs the setting keeps"
    assert 7000 < ch["refresh_in_s"] <= 7200, "and again in two hours, as the setting says"
    status = rig.wait("some GIFs downloaded and one playing",
                      lambda s: s["downloads"] >= before["downloads"] + 3 and s["playing"] and not s["loading"],
                      timeout=120)
    assert status["current_source"] == "giphy" and len(status["current"]) >= 6
    assert channel(status)["available"] >= 3 and status["download_failures"] == before["download_failures"]
    width, height, frames = rig.on_panel()
    assert height == 200 and width > 0 and frames > 1, "the 200-pixel-high rendition, animated"
    # The key travels in the address of every request. It is in no log line.
    lines = rig.api.get("/api/v2/test/log").body
    assert b"giphy" in lines and rig.key.encode() not in lines


def test_a_search_channel_lists_its_query(rig):
    before = rig.status()
    assert rig.uart.command("sched activate", playset("search", "pixel art", offset=int(time.time()) % 450)) == {"result": "ok"}
    status = rig.wait("listed", lambda s: s["playset"] == "Giphy test" and channel(s)["listings"] >= 1
                      and channel(s)["state"] == "ready", timeout=60)
    assert 16 <= channel(status)["records"] <= 32
    status = rig.wait("playing a result", lambda s: s["downloads"] > before["downloads"] and s["playing"]
                      and not s["loading"], timeout=120)
    assert status["current_source"] == "giphy"

    # A kind the source does not have is an error of the channel, not of the device.
    assert rig.uart.command("sched activate", playset("stickers")) == {"result": "ok"}
    status = rig.wait("refused", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] == "failed")
    assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED"


def log_since(rig, mark: int) -> tuple:
    data = rig.api.get(f"/api/v2/test/log?since={mark}").data
    return " ".join(str(line) for line in data["lines"]), data["next"]


def test_a_swipe_up_on_a_gif_is_told_to_giphy(rig):
    assert rig.uart.command("sched activate", playset("trending")) == {"result": "ok"}
    rig.wait("a GIF is playing", lambda s: s["playing"] and not s["loading"] and s["current_source"] == "giphy",
             timeout=60)
    _, mark = log_since(rig, 0)
    swipe = [[1, 360, 500 - 30 * i, 0, 0, 30] for i in range(9)] + [[0, 0, 0, 0, 0, 60]]
    assert rig.uart.command("ui touch", swipe) == {"result": "ok"}
    deadline = time.monotonic() + 30
    text = ""
    while "Giphy" not in text and time.monotonic() < deadline:
        time.sleep(0.5)
        text, _ = log_since(rig, mark)
    assert rig.uart.command("ui touched")["last_action"] == "like"
    assert "told to Giphy" in text and "not told" not in text, text[-300:]

    # The same swipe pins it into the active list, and a swipe down takes it out again.
    def pinned():
        return [entry.get("id") for entry in rig.api.get("/api/v2/pins/active").data["pins"]]
    shown = rig.status()["current"]
    deadline = time.monotonic() + 10
    while shown not in pinned() and time.monotonic() < deadline:
        time.sleep(0.3)
    assert shown in pinned()
    down = [[1, 360, 260 + 30 * i, 0, 0, 30] for i in range(9)] + [[0, 0, 0, 0, 0, 60]]
    before = rig.uart.command("ui touched")
    assert rig.uart.command("ui touch", down) == {"result": "ok"}
    deadline = time.monotonic() + 5
    touched = rig.uart.command("ui touched")
    while touched["scripted"] and time.monotonic() < deadline:
        time.sleep(0.1)
        touched = rig.uart.command("ui touched")
    assert touched["gestures"] == before["gestures"] + 1 and touched["last_action"] == "unlike", (before, touched)
    deadline = time.monotonic() + 10
    while shown in pinned() and time.monotonic() < deadline:
        time.sleep(0.3)
    assert shown not in pinned()
    assert rig.uart.command("ui touched")["last_action"] == "unlike"


def test_the_id_giphy_gave_can_be_forgotten_and_a_new_one_is_asked_for(rig):
    _, mark = log_since(rig, 0)
    assert rig.api.call("POST", "/api/v2/sources/giphy/reset-id").status == 200
    assert rig.api.call("POST", "/api/v2/sources/giphy/reset-id").status == 200, "nothing to forget is not an error"
    before = channel(rig.status())["listings"]
    assert rig.api.call("POST", "/api/v2/player/refresh").status == 200
    rig.wait("listed again", lambda s: channel(s)["listings"] > before and channel(s)["state"] == "ready", timeout=60)
    text, _ = log_since(rig, mark)
    assert "Giphy gave this device an id" in text


def test_a_refused_key_is_said_over_what_plays_until_it_is_put_right(rig):
    from test_decoders import CORPUS
    rig.put("zz-giphy-local.png", (CORPUS / "good" / "png/rgb.png").read_bytes())
    try:
        both = {"name": "Mixed", "channels": [{"source": "local"},
                                              {"source": "giphy", "kind": "search", "identifier": "p3a", "offset": RUN}]}
        rig.uart.command("settings set", {"giphy_api_key": "0000000000000000"})
        assert rig.uart.command("sched activate", both) == {"result": "ok"}
        rig.wait("the user's file plays and Giphy has refused",
                 lambda s: s["playing"] and not s["loading"] and s["channels"][1].get("error") == "AUTH", timeout=60)
        deadline = time.monotonic() + 10
        ui = rig.api.get("/api/v2/ui").data
        while ui.get("notice") != "source_problem" and time.monotonic() < deadline:
            time.sleep(0.3)
            ui = rig.api.get("/api/v2/ui").data
        assert ui["screen"] == "artwork" and ui["banner"] is True and ui["notice"] == "source_problem", ui
        assert ui["text"] == "Giphy refused the API key. Check it on the web page"
        # The information screen has a row for each remote source of the playset.
        from test_touch import long_press

        def press():  # a long press takes 2.4 seconds: wait for the script to end
            assert rig.uart.command("ui touch", long_press()) == {"result": "ok"}
            deadline = time.monotonic() + 8
            while rig.uart.command("ui touched")["scripted"] and time.monotonic() < deadline:
                time.sleep(0.1)
            time.sleep(0.3)

        press()
        info = rig.api.get("/api/v2/ui").data
        rows = dict(line.split("\t") for line in info["text"].split("\n"))
        assert info["screen"] == "info" and rows["Giphy"] == "Key refused", rows
        press()
        assert rig.api.get("/api/v2/ui").data["screen"] == "artwork"
        # The right key: the channel is listed by itself and the notice goes.
        rig.uart.command("settings set", {"giphy_api_key": rig.key})
        rig.wait("giphy listed", lambda s: s["channels"][1]["state"] == "ready" and "error" not in s["channels"][1],
                 timeout=60)
        deadline = time.monotonic() + 10
        while rig.api.get("/api/v2/ui").data.get("notice") == "source_problem" and time.monotonic() < deadline:
            time.sleep(0.3)
        assert rig.api.get("/api/v2/ui").data.get("notice") != "source_problem"
    finally:
        rig.uart.command("sched activate", LOCAL_ONLY)
        rig.remove("zz-giphy-local.png")


def test_giphy_is_named_in_the_band_under_its_gifs(rig):
    import io
    import pathlib
    import struct
    from PIL import Image
    # The mark as the firmware holds it, mixed with black as the device mixes it.
    mark_file = (pathlib.Path(__file__).parents[2] / "components" / "source_giphy" / "credit"
                 / "powered-by-giphy.p3aimg").read_bytes()
    mark_w, mark_h = struct.unpack_from("<HH", mark_file, 6)
    expected = Image.new("RGB", (mark_w, mark_h))
    expected.putdata([tuple((mark_file[10 + i * 4 + c] * mark_file[13 + i * 4] + 127) // 255 for c in (2, 1, 0))
                      for i in range(mark_w * mark_h)])
    last = {}

    def band_pixels() -> tuple:
        """(rows in the band below the picture, pixels that are not the background in it and above the picture)

        Four rows next to the picture are left out on each side: the player may place a scaled picture's edge a
        row or two from where this works it out, and the mark keeps four rows clear of the picture anyway."""
        width, height, _ = rig.on_panel()
        shown_h = 720 * height // width if width >= height else 720
        band_top = (720 - shown_h) // 2 + shown_h
        shot = Image.open(io.BytesIO(rig.api.get("/api/v2/test/screenshot", timeout=60).body)).convert("RGB")
        last.update(shot=shot, band_top=band_top)
        count = lambda box: sum(n for n, colour in (shot.crop(box).getcolors(1 << 20) or []) if colour != (0, 0, 0))
        return 720 - band_top, count((0, band_top + 4, 720, 720)) if band_top + 4 < 720 else 0, \
            count((0, 0, 720, (720 - shown_h) // 2 - 4)) if shown_h < 720 - 8 else 0

    # The clock is drawn in a band too: off while the bands are looked at.
    clock = rig.api.get("/api/v2/settings").data["clock_enabled"]
    rig.uart.command("settings set", {"background_color": "#000000", "rotation": 0, "clock_enabled": False})
    try:
        assert rig.uart.command("sched activate", playset("trending")) == {"result": "ok"}
        rig.wait("a GIF is playing", lambda s: s["playing"] and not s["loading"] and s["current_source"] == "giphy",
                 timeout=60)
        # A GIF wide enough to leave a band: most are, a square one is not.
        for _ in range(8):
            rows, below, above = band_pixels()
            if rows >= 60:
                break
            rig.step("next")
        assert rows >= 60, "no wide GIF among those fetched"
        time.sleep(1.0)
        rows, below, above = band_pixels()
        assert below > 200 and above == 0, "the mark is in the band below the picture, and only there"
        # Giphy's own mark, pixel for pixel, against the right and in the middle of the band's height.
        left, top = 720 - 12 - mark_w, last["band_top"] + (rows - mark_h) // 2
        assert last["shot"].crop((left, top, left + mark_w, top + mark_h)).tobytes() == expected.tobytes()
        assert last["shot"].crop((0, last["band_top"] + 4, left, 720)).getcolors() == [(left * (rows - 4), (0, 0, 0))], \
            "nothing left of the mark"
        # Under the first GIF after another source's artwork too: the mark and the picture change
        # together then, and the credit once went missing until the next change.
        picture = (pathlib.Path(__file__).parents[1] / "assets" / "decoders" / "good" / "png" / "gray.png").read_bytes()
        try:
            assert rig.api.call("POST", "/api/v2/files?name=zz-giphy-between.png", picture, timeout=30).status == 200
            rig.wait("the card's picture is up", lambda s: s["current_source"] == "local" and not s["loading"])
            time.sleep(1.0)
            assert band_pixels()[1] == 0, "a picture from the card has no credit"
            rig.step("back")
            assert rig.status()["current_source"] == "giphy"
            time.sleep(1.0)
            assert band_pixels()[1] > 200, "the GIF that comes back has its credit"
        finally:
            rig.remove("zz-giphy-between.png")
        rig.uart.command("settings set", {"source_credit": False})
        try:
            time.sleep(1.0)
            assert band_pixels()[1] == 0, "and gone when the setting says so"
        finally:
            rig.uart.command("settings set", {"source_credit": True})
    finally:
        rig.uart.command("settings set", {"clock_enabled": clock})
