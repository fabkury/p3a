# SPDX-License-Identifier: Apache-2.0
"""The museum source on the device, against the museums themselves: a channel
is listed, its pictures are fetched from the museum's image server at the
panel's size, and they play.

Reading each museum's listing is host-tested on recorded responses
(test_museums.c); this is the wiring, the TLS session with each museum's
servers, and the pictures themselves.
"""
import time

import pytest

import lab
from test_api import Api
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live

RUN = int(time.time()) % 3000  # a channel the device has not seen: see test_giphy.py

# museum, kind, a term with thousands of artworks, the longest side its pictures may have
CHANNELS = [
    ("vam", "vam:category", "THES48903", 720),          # prints
    ("smk", "smk:collection", "Gammel bestand", 720),
    ("wellcome", "wellcome:workType", "k", 720),         # pictures
    ("cma", "cma:department", "Drawings", 1400),         # no IIIF: one fixed "web" size
    ("mia", "mia:classification", "Paintings", 800),     # no IIIF: the 800-pixel bucket
]


def playset(kind: str, term: str, offset: int = RUN) -> dict:
    return {"name": "Museum test", "channels": [{"source": "museum", "kind": kind, "identifier": term,
                                                 "offset": offset}]}


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    uart.command("settings set", {"museum_cache_size": 32, "dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"museum_cache_size": 1024, "dwell_s": 30})
    uart.command("library evict 1000000000000 0 -", timeout=120)


def channel(status):
    return status["channels"][0]


@pytest.mark.parametrize("museum,kind,term,longest", CHANNELS, ids=[c[0] for c in CHANNELS])
def test_a_museums_channel_is_listed_fetched_and_played(rig, museum, kind, term, longest):
    before = rig.status()
    assert rig.uart.command("sched activate", playset(kind, term)) == {"result": "ok"}
    status = rig.wait("listed", lambda s: s["playset"] == "Museum test" and channel(s)["listings"] >= 1
                      and channel(s)["state"] != "listing", timeout=90)
    ch = channel(status)
    assert ch["state"] == "ready" and "error" not in ch, ch
    assert ch["records"] == 32, "as many as the setting keeps, from a term with thousands"
    assert ch["refresh_in_s"] > 86000, "museums change slowly: listed again after days"
    status = rig.wait("some pictures fetched and one playing",
                      lambda s: s["downloads"] >= before["downloads"] + 3 and s["playing"] and not s["loading"]
                      and s["current_source"] == "museum", timeout=180)
    # Minneapolis lists some works whose pictures it then refuses (about one in twenty).
    assert status["download_failures"] - before["download_failures"] <= (2 if museum == "mia" else 0)
    width, height, frames = rig.on_panel()
    assert frames == 1 and max(width, height) <= longest, "a still, of the size asked for"
    assert max(width, height) >= 300, "and not a thumbnail"
    shown = rig.api.get("/api/v2/player").data["current"]
    assert shown["source"] == "museum" and shown["format"] == "jpg" and shown["id"] == status["current"]


def test_a_museum_or_an_axis_this_firmware_lacks_is_the_channels_error(rig):
    for kind in ("louvre:collection", "vam:department", "vam"):
        assert rig.uart.command("sched activate", playset(kind, "x")) == {"result": "ok"}
        status = rig.wait("refused", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] == "failed")
        assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED", kind
