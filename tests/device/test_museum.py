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
    ("ham", "ham:classification", "26", 720),            # paintings; needs Harvard's key
    ("si", "si:unit", "SAAM", 720),                      # the American Art Museum; needs the Smithsonian's key
    ("rijks", "rijks:set", "26118", 720),                # each picture's address is asked for in three steps
]
# museum -> (the setting its key goes in, the key's name in the secrets file)
KEYS = {"ham": ("harvard_api_key", "ham_api_key"), "si": ("smithsonian_api_key", "si_api_key")}


def playset(kind: str, term: str, offset: int = RUN) -> dict:
    return {"name": "Museum test", "channels": [{"source": "museum", "kind": kind, "identifier": term,
                                                 "offset": offset}]}


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    uart.command("settings set", {"museum_cache_size": 32, "dwell_s": 600, "harvard_api_key": "",
                                  "smithsonian_api_key": ""})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"museum_cache_size": 1024, "dwell_s": 30, "harvard_api_key": "",
                                  "smithsonian_api_key": ""})
    uart.command("library evict 1000000000000 0 -", timeout=120)


def channel(status):
    return status["channels"][0]


@pytest.mark.parametrize("museum,kind,term,longest", CHANNELS, ids=[c[0] for c in CHANNELS])
def test_a_museums_channel_is_listed_fetched_and_played(rig, museum, kind, term, longest):
    before = rig.status()
    assert rig.uart.command("sched activate", playset(kind, term)) == {"result": "ok"}
    if museum in KEYS:
        setting, secret = KEYS[museum]
        # Without its key the channel says so and the museum is not asked.
        status = rig.wait("refused for want of a key", lambda s: s["playset"] == "Museum test"
                          and channel(s)["listings"] >= 1 and channel(s)["state"] != "listing", timeout=30)
        assert channel(status)["error"] == "NO_KEY" and channel(status)["state"] == "failed"
        # A key the museum does not know is told apart from no key at all.
        listings = channel(status)["listings"]
        rig.uart.command("settings set", {setting: "00000000-0000-0000-0000-000000000000"})
        status = rig.wait("the museum refused it", lambda s: channel(s)["listings"] > listings
                          and channel(s)["state"] != "listing", timeout=60)
        assert channel(status)["error"] == "AUTH", channel(status)
        # The right key: saving it lists the channel by itself.
        listings = channel(status)["listings"]
        rig.uart.command("settings set", {setting: lab.load_secrets()["api_keys"][secret]})
        rig.wait("listed with the key", lambda s: channel(s)["listings"] > listings and channel(s)["state"] != "listing",
                 timeout=90)
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
    if museum in KEYS:
        lines = rig.api.get("/api/v2/test/log").body
        assert lab.load_secrets()["api_keys"][KEYS[museum][1]].encode() not in lines, "the key is in no log line"


def test_the_firmware_lists_its_museums(rig):
    rig.uart.command("settings set", {"harvard_api_key": "", "smithsonian_api_key": ""})  # the tests above set them
    museums = {m["id"]: m for m in rig.api.get("/api/v2/sources/museums").data["museums"]}
    assert set(museums) == {"vam", "smk", "wellcome", "cma", "mia", "ham", "si", "rijks", "artic"}
    assert museums["vam"] == {"id": "vam", "name": "Victoria and Albert Museum",
                              "axes": ["collection", "category", "venue"], "needs_key": False}
    assert museums["ham"]["needs_key"] is True and museums["ham"]["key_set"] is False
    assert "unavailable" in museums["artic"] and "unavailable" not in museums["rijks"]
    # Every channel the tests play is one the list offers.
    for museum, kind, _term, _longest in CHANNELS:
        assert kind.split(":")[1] in museums[museum]["axes"]
    rig.uart.command("settings set", {"harvard_api_key": "x"})
    try:
        again = {m["id"]: m for m in rig.api.get("/api/v2/sources/museums").data["museums"]}
        assert again["ham"]["key_set"] is True and again["si"]["key_set"] is False
    finally:
        rig.uart.command("settings set", {"harvard_api_key": ""})


def test_a_museum_that_cannot_be_played_now_is_not_asked(rig):
    """The Art Institute of Chicago's image server refuses clients that are not web browsers. Its
    channels say they cannot be played, and the test asks the server from the device whether that
    is still so: the day this fails, the museum can come back (museum_artic.c)."""
    assert rig.uart.command("sched activate", playset("artic:departments", "PC-10")) == {"result": "ok"}
    status = rig.wait("refused", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] == "failed")
    assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED"
    picture = "https://www.artic.edu/iiif/2/6644829f-f292-c5c4-a73c-0356a6fdbf0d/full/!720,720/0/default.jpg"
    got = rig.uart.command("fetch get", {"url": picture}, timeout=90)
    assert got["result"] == "AUTH" and got["status"] == 403, f"the image server no longer refuses the device: {got}"


def test_a_museum_or_an_axis_this_firmware_lacks_is_the_channels_error(rig):
    for kind in ("louvre:collection", "vam:department", "vam"):
        assert rig.uart.command("sched activate", playset(kind, "x")) == {"result": "ok"}
        status = rig.wait("refused", lambda s: channel(s)["listings"] >= 1 and channel(s)["state"] == "failed")
        assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED", kind


def test_changing_the_playset_does_not_wait_for_a_long_listing_to_end(rig):
    """Found by the suite: a museum channel of a thousand artworks takes eleven requests to list,
    and the next playset's channels waited behind it."""
    from test_decoders import CORPUS
    rig.put("zz-museum-local.png", (CORPUS / "good" / "png/rgb.png").read_bytes())
    rig.uart.command("settings set", {"museum_cache_size": 1024})
    try:
        assert rig.uart.command("sched activate", playset("cma:department", "Drawings", offset=RUN + 7)) == {"result": "ok"}
        rig.wait("the long listing has begun", lambda s: channel(s)["state"] == "listing", timeout=10)
        time.sleep(3.0)
        assert channel(rig.status())["state"] == "listing", "still at it: a thousand artworks are many pages"
        started = time.monotonic()
        assert rig.uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
        rig.wait("the new playset's channel is listed and playing",
                 lambda s: s["playset"] == "" and channel(s)["state"] == "ready" and s["playing"] and not s["loading"]
                 and s["current_source"] == "local", timeout=10)
        assert time.monotonic() - started < 8, "within the time of one request, not of the whole listing"
    finally:
        rig.uart.command("settings set", {"museum_cache_size": 32})
        rig.remove("zz-museum-local.png")
