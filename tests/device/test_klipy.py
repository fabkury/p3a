# SPDX-License-Identifier: Apache-2.0
"""The Klipy source on the device, against Klipy itself: a channel without a
key, with a key Klipy does not know (which it answers 404), and with the
test rig's key (from the secrets file): listed, each item's address asked
for, downloaded, played.

Reading the answers is host-tested on recorded responses
(test_klipy_parse.c); this is the wiring and the live service. Every
download costs a request of the key's allowance, so the test stops the
channel after a few.
"""
import time

import pytest

import lab
from test_api import Api
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live

RUN = int(time.time()) % 2000  # see test_giphy.py: a channel the device has not seen


def playset(kind: str, identifier: str = "", offset: int = RUN) -> dict:
    return {"name": "Klipy test", "channels": [{"source": "klipy", "kind": kind, "identifier": identifier,
                                               "offset": offset}]}


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    r.key = lab.load_secrets()["api_keys"]["klipy_api_key"]
    uart.command("settings set", {"klipy_api_key": "", "klipy_cache_size": 32, "klipy_rating": "g",
                                  "klipy_format": "gif", "dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"klipy_api_key": "", "klipy_cache_size": 350, "klipy_rating": "pg-13",
                                  "dwell_s": 30})
    uart.command("library evict 1000000000000 0 -", timeout=120)


def channel(status):
    return status["channels"][0]


def test_without_a_key_and_with_one_klipy_does_not_know(rig):
    assert rig.uart.command("sched activate", playset("gif:trending")) == {"result": "ok"}
    status = rig.wait("the listing answered", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] != "listing")
    assert channel(status)["source"] == "klipy" and channel(status)["error"] == "NO_KEY"

    before = channel(status)["listings"]
    rig.uart.command("settings set", {"klipy_api_key": "0000000000000000"})
    status = rig.wait("klipy answered", lambda s: channel(s)["listings"] > before and channel(s)["state"] != "listing",
                      timeout=60)
    assert channel(status)["error"] == "AUTH", "Klipy's 404 for an unknown key is a refused key, not a missing channel"
    assert channel(status)["state"] == "failed" and 3500 < channel(status)["refresh_in_s"] <= 3600


def test_with_a_key_trending_gifs_are_listed_downloaded_and_played(rig):
    before = rig.status()
    rig.uart.command("settings set", {"klipy_api_key": rig.key})
    status = rig.wait("listed", lambda s: channel(s)["listings"] > channel(before)["listings"]
                      and channel(s)["state"] == "ready", timeout=60)
    assert "error" not in channel(status) and 16 <= channel(status)["records"] <= 32
    status = rig.wait("some GIFs downloaded and one playing",
                      lambda s: s["downloads"] >= before["downloads"] + 2 and s["playing"] and not s["loading"],
                      timeout=120)
    assert status["current_source"] == "klipy" and status["download_failures"] == before["download_failures"]
    width, height, frames = rig.on_panel()
    assert width > 0 and height > 0 and frames > 1
    # The key is part of the path of every request to Klipy. It is in no log line.
    lines = rig.api.get("/api/v2/test/log").body
    assert b"klipy" in lines and rig.key.encode() not in lines


def test_klipy_is_credited_with_its_mark_under_its_gifs(rig):
    """Follows the test above: Klipy's trending GIFs are playing. The rules of the mark are test_giphy.py's."""
    import io
    import pathlib
    import struct
    from PIL import Image
    data = (pathlib.Path(__file__).parents[2] / "components" / "source_klipy" / "credit"
            / "powered-by-klipy-on-dark.p3aimg").read_bytes()
    mark_w, mark_h = struct.unpack_from("<HH", data, 6)
    expected = Image.new("RGB", (mark_w, mark_h))
    expected.putdata([tuple((data[10 + i * 4 + c] * data[13 + i * 4] + 127) // 255 for c in (2, 1, 0))
                      for i in range(mark_w * mark_h)])
    settings = rig.api.get("/api/v2/settings").data
    rig.uart.command("settings set", {"background_color": "#000000", "rotation": 0, "clock_enabled": False,
                                      "source_credit": True})
    try:
        # A GIF wide enough to leave a band: most are, a square one is not.
        for _ in range(8):
            rig.wait("a GIF is playing", lambda s: s["playing"] and not s["loading"] and s["current_source"] == "klipy",
                     timeout=60)
            width, height, _ = rig.on_panel()
            shown_h = 720 * height // width if width >= height else 720
            band_top = (720 - shown_h) // 2 + shown_h
            if 720 - band_top >= 60:
                break
            rig.step("next")
        assert 720 - band_top >= 60, "no wide GIF among those fetched"
        time.sleep(1.0)
        shot = Image.open(io.BytesIO(rig.api.get("/api/v2/test/screenshot", timeout=60).body)).convert("RGB")
        left, top = 720 - 12 - mark_w, band_top + (720 - band_top - mark_h) // 2
        assert shot.crop((left, top, left + mark_w, top + mark_h)).tobytes() == expected.tobytes()
    finally:
        rig.uart.command("settings set", {key: settings[key] for key in ("background_color", "rotation", "clock_enabled",
                                                                         "source_credit")})


def test_a_sticker_search_as_webp(rig):
    before = rig.status()
    rig.uart.command("settings set", {"klipy_format": "webp"})
    assert rig.uart.command("sched activate", playset("sticker:search", "pixel art", offset=RUN % 50)) == {"result": "ok"}
    status = rig.wait("listed", lambda s: s["playset"] == "Klipy test" and channel(s)["listings"] >= 1
                      and channel(s)["state"] == "ready", timeout=60)
    assert 16 <= channel(status)["records"] <= 32
    status = rig.wait("playing a sticker", lambda s: s["downloads"] > before["downloads"] and s["playing"]
                      and not s["loading"] and s["current_source"] == "klipy", timeout=120)
    shown = rig.api.get("/api/v2/player").data["current"]
    assert shown["source"] == "klipy" and shown["format"] == "webp", shown

    assert rig.uart.command("sched activate", playset("clip:trending")) == {"result": "ok"}
    status = rig.wait("refused", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] == "failed")
    assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED"
