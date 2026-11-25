# SPDX-License-Identifier: Apache-2.0
"""The content task on the device: a remote channel is listed, its artworks
are downloaded into the blob store and played, a fresh listing keeps what
was already fetched, failures are told apart, and after a restart the
channel plays from its index on the card.

The source is the test hooks' own ("test"): the test says what it lists and
how a listing ends. The pictures are real files on a server that has served
them unchanged for years. When to list again and the waits after a failure
are host-tested (test_sched_refresh.c); this is the wiring.
"""
import hashlib
import time
import urllib.request

import pytest

import lab
from test_scheduler import LOCAL_ONLY, Rig
from test_api import Api

pytestmark = pytest.mark.live

GALLERY = "https://www.gstatic.com/webp/gallery/"
PICTURES = {"a": "1.webp", "b": "2.webp", "c": "4.webp"}
# A channel of its own for every run: a channel the device has seen before has its index on the
# card and is not listed again until that is an hour old.
REMOTE = {"name": "Remote", "channels": [{"source": "test", "kind": f"run{int(time.time())}"}]}
TEST_SOURCE = 7
WEBP = 1


def items(*ids, **extra):
    listing = [{"id": i, "url": GALLERY + PICTURES[i]} for i in ids]
    return listing + [{"id": i, **({"url": u} if u else {})} for i, u in extra.items()]


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    refs = []
    for name in PICTURES.values():
        with urllib.request.urlopen(GALLERY + name, timeout=30) as reply:
            refs.append(hashlib.sha256(reply.read()).hexdigest()[:32])
    for ref in refs:
        uart.command(f"library blob-remove {ref} {WEBP}")  # left by a run that did not finish
    uart.command("settings set", {"dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    for ref in refs:
        uart.command(f"library blob-remove {ref} {WEBP}")
    uart.command("settings set", {"dwell_s": 30})


def channel(status):
    return status["channels"][0]


def relist(rig, what, test, timeout=60.0):
    before = channel(rig.status())["listings"]
    assert rig.uart.command(f"sched refresh {TEST_SOURCE}") == {"result": "ok"}
    return rig.wait(what, lambda s: channel(s)["listings"] > before and channel(s)["state"] != "listing" and test(s),
                    timeout=timeout)


def test_a_remote_channel_is_listed_downloaded_and_played(rig):
    assert rig.uart.command("content set", {"items": items("a", "b"), "result": "ok"}) == {"items": 2}
    before = rig.status()
    asked = rig.uart.command("content listed")["listed"]
    assert rig.uart.command("sched activate", REMOTE) == {"result": "ok"}
    status = rig.wait("both artworks downloaded and one playing",
                      lambda s: s["playset"] == "Remote" and channel(s)["available"] == 2 and s["playing"]
                      and not s["loading"] and not s["downloading"], timeout=90)
    ch = channel(status)
    assert ch["state"] == "ready" and ch["records"] == 2 and ch["source"] == "test"
    assert status["downloads"] == before["downloads"] + 2 and status["download_failures"] == before["download_failures"]
    assert status["current_source"] == "giphy" and status["current"] in ("a", "b")
    assert rig.on_panel()[2] == 1, "a real picture is on the panel"
    assert rig.uart.command("content listed")["listed"] == asked + 1, "listed once, not once per download"
    assert 3500 < ch["refresh_in_s"] <= 3600, "and again in an hour, as the source asks"


def test_a_fresh_listing_keeps_what_was_already_fetched(rig):
    before = rig.status()
    rig.uart.command("content set", {"items": items("c", "b")})
    relist(rig, "the new listing in place", lambda s: channel(s)["records"] == 2)
    status = rig.wait("the one new artwork downloaded", lambda s: channel(s)["available"] == 2 and not s["downloading"],
                      timeout=90)
    assert status["downloads"] == before["downloads"] + 1, "b was not fetched again"


def test_an_artwork_the_source_no_longer_has_is_not_asked_for_again(rig):
    before = rig.status()
    rig.uart.command("content set", {"items": items("c", d=GALLERY + "no-such-p3a-file.webp", e=None)})
    relist(rig, "listed", lambda s: channel(s)["records"] == 3)
    status = rig.wait("both tried", lambda s: s["download_failures"] == before["download_failures"] + 2
                      and not s["downloading"], timeout=90)
    assert channel(status)["available"] == 1
    time.sleep(5)
    after = rig.status()
    assert after["download_failures"] == status["download_failures"], "gone is final"
    assert after["downloads"] == before["downloads"]


def test_a_listing_that_fails_keeps_the_channel_playing_and_waits(rig):
    rig.uart.command("content set", {"result": "RATE_LIMITED"})
    status = relist(rig, "the refusal reported", lambda s: channel(s).get("error") == "RATE_LIMITED")
    ch = channel(status)
    assert ch["state"] == "ready" and ch["available"] == 1, "what it had stays playable"
    assert 850 < ch["refresh_in_s"] <= 900, "a rate limit waits a quarter of an hour"

    rig.uart.command("content set", {"result": "NO_KEY"})
    status = relist(rig, "the missing key reported", lambda s: channel(s).get("error") == "NO_KEY")
    assert 3500 < channel(status)["refresh_in_s"] <= 3600

    # A change of settings may be the key arriving: the channel is listed again without being asked.
    rig.uart.command("content set", {"result": "ok"})
    listings = channel(status)["listings"]
    rig.uart.command("settings set", {"dwell_s": 601})
    status = rig.wait("listed again by itself", lambda s: channel(s)["listings"] > listings
                      and channel(s)["state"] == "ready" and "error" not in channel(s), timeout=30)
    assert channel(status)["records"] == 3


def test_something_that_is_not_a_picture_is_not_kept(rig):
    before = rig.status()
    rig.uart.command("content set", {"items": items("c", f="https://example.com/")})
    relist(rig, "listed", lambda s: channel(s)["records"] == 2)
    status = rig.wait("the page was fetched and refused",
                      lambda s: s["download_failures"] > before["download_failures"] and not s["downloading"], timeout=60)
    assert channel(status)["available"] == 1 and status["downloads"] == before["downloads"]


def test_listing_and_downloading_wait_while_internal_memory_is_short(rig):
    uart = rig.uart
    uart.command("content set", {"items": items("c", "a", "b")})
    before = rig.status()
    base = uart.command("mem release")
    low = uart.command(f"mem hog {base['free'] - 52_000}")
    try:
        assert low["level"] == "low", low
        assert uart.command(f"sched refresh {TEST_SOURCE}") == {"result": "ok"}
        time.sleep(6)
        held = rig.status()
        assert channel(held)["listings"] == channel(before)["listings"], "a listing over the network is optional work"
        assert held["downloads"] == before["downloads"] and not held["downloading"]
        assert held["playing"], "what is on the card plays on"
    finally:
        assert uart.command("mem release")["level"] == "ok"
    status = rig.wait("listed and fetched once memory is back",
                      lambda s: channel(s)["listings"] > channel(before)["listings"] and channel(s)["available"] == 3
                      and not s["downloading"], timeout=90)
    assert status["downloads"] == before["downloads"] + 2


def test_a_person_can_ask_for_fresh_listings_now(rig):
    before = channel(rig.status())
    assert before["refresh_in_s"] > 3000, "not due for most of an hour"
    assert rig.api.call("POST", "/api/v2/player/refresh").status == 200
    status = rig.wait("listed again", lambda s: channel(s)["listings"] > before["listings"]
                      and channel(s)["state"] == "ready", timeout=30)
    assert channel(status)["records"] == 3
    shown = rig.api.get("/api/v2/player").data
    assert shown["channels"][0]["to_download"] == 0 and shown["downloading"] is False


def test_when_room_is_needed_the_oldest_blobs_go_and_an_artwork_that_went_is_fetched_again(rig):
    GIF, NOTHING, ALL = 2, "-", 10**12
    uart = rig.uart

    def evict(need, min_age_s, keep):
        return uart.command(f"library evict {need} {min_age_s} {keep}", timeout=120)

    def has(ref):
        return uart.command(f"library blob-has {ref} {GIF}")["has"]

    # An empty store first, so that the three blobs made here are all there is.
    assert evict(ALL, 0, NOTHING)["result"] == "ok"
    refs = []
    for seed in (901, 902, 903):
        refs.append(uart.command(f"library blob 5000 {seed} {GIF}", timeout=30)["ref"])
        time.sleep(3)  # FAT keeps times to two seconds
    oldest, middle, newest = refs

    done = evict(1, 0, oldest)
    assert done["seen"] == 3 and done["deleted"] == 1 and done["freed"] == 5000, "one byte is wanted: one blob goes"
    assert has(oldest) and not has(middle) and has(newest), "the oldest that may go, not the one that is kept"
    assert evict(ALL, 3600, NOTHING)["deleted"] == 0, "nothing younger than the minimum age goes, whatever is wanted"
    done = evict(ALL, 0, oldest)
    assert done["deleted"] == 1 and has(oldest) and not has(newest)
    assert evict(ALL, 0, NOTHING)["deleted"] == 1 and not has(oldest)

    # The channel's own artworks went with the first sweep. The index still names them: each is
    # found out when it is played, and fetched again.
    before = rig.status()
    assert channel(before)["available"] >= 1
    assert uart.command("sched next") == {"result": "ok"}
    status = rig.wait("fetched again and playing", lambda s: s["downloads"] > before["downloads"] and s["playing"]
                      and not s["loading"] and not s["downloading"], timeout=90)
    assert channel(status)["available"] >= 1


def test_after_a_restart_the_channel_plays_from_its_index_on_the_card(rig):
    rig.uart.command("content set", {"items": items("a", "b", "c")})
    relist(rig, "listed", lambda s: channel(s)["records"] == 3)
    rig.wait("all three on the card", lambda s: channel(s)["available"] == 3 and not s["downloading"], timeout=120)
    # A listing is saved at once; an index changed by downloads half a minute later. Listing again saves it now.
    relist(rig, "listed again", lambda s: channel(s)["available"] == 3)
    time.sleep(3)

    start = rig.uart.mark()
    rig.uart.command("reboot")
    rig.uart.wait_ready(since=start, timeout=30)
    # After the restart the test source has no listing: a service that cannot be reached.
    status = rig.wait("playing what it knew", lambda s: s["playing"] and not s["loading"], timeout=30)
    ch = channel(status)
    assert status["playset"] == "Remote" and ch["records"] == 3 and ch["available"] == 3
    assert status["current"] in PICTURES and status["downloads"] == 0, "nothing was fetched again"
    lab.wait_online(rig.uart)
    status = rig.wait("the listing was tried and failed", lambda s: channel(s)["listings"] >= 1, timeout=60)
    assert channel(status)["state"] == "ready" and channel(status)["available"] == 3, "and the channel plays on"


def test_a_playset_whose_blobs_all_went_starts_when_the_first_is_fetched_again(rig):
    """Found with Klipy: the index on the card said its artworks were there, the first picks
    failed, and the previous playset's artwork stayed up for a whole dwell."""
    from test_decoders import CORPUS
    uart = rig.uart
    # The test before this one restarted the device: the test source is told its listing again.
    uart.command("content set", {"items": items("a", "b", "c"), "result": "ok"})
    relist(rig, "listed", lambda s: channel(s)["records"] == 3)
    rig.wait("the channel has its artworks", lambda s: channel(s)["available"] == 3 and not s["downloading"], timeout=120)
    relist(rig, "its index saved with them", lambda s: channel(s)["available"] == 3)
    time.sleep(3)
    rig.put("zz-content-local.png", (CORPUS / "good" / "png/rgb.png").read_bytes())
    try:
        assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
        uart.command("sched refresh 1")
        rig.wait("the user's file plays", lambda s: s["playing"] and not s["loading"] and s["current_source"] == "local",
                 timeout=20)
        assert uart.command("library evict 1000000000000 0 -", timeout=120)["deleted"] >= 3
        before = rig.status()
        started = time.monotonic()
        assert uart.command("sched activate", REMOTE) == {"result": "ok"}
        status = rig.wait("an artwork of the new playset is up",
                          lambda s: s["playset"] == "Remote" and s["playing"] and not s["loading"]
                          and s["current_source"] == "giphy", timeout=60)
        assert status["downloads"] > before["downloads"], "fetched again, since the store no longer had it"
        assert time.monotonic() - started < 60, "at once, not when the old artwork's ten minutes were up"
    finally:
        rig.remove("zz-content-local.png")


def test_a_very_elongated_picture_is_passed_over_unless_the_filter_is_off(rig):
    """Item D on the device. The elongated picture is a 5:1 strip cut from a museum's image by its
    IIIF server (720 by 144); the filter's limit is 4:1."""
    uart = rig.uart
    strip = "https://framemark.vam.ac.uk/collections/2016JL6674/0,0,2000,400/!720,720/0/default.jpg"
    uart.command("settings set", {"aspect_filter": True, "aspect_max_tenths": 40})
    uart.command("content set", {"items": items("a", strip=strip), "result": "ok"})
    relist(rig, "listed", lambda s: channel(s)["records"] == 2)
    rig.wait("both fetched", lambda s: channel(s)["available"] == 2 and not s["downloading"], timeout=90)
    mark = rig.api.get("/api/v2/test/log").data["next"]
    seen = set()
    for _ in range(8):
        status = rig.step("next")
        seen.add(status["current"])
    assert seen == {"a"}, "the strip is never shown"
    text = " ".join(str(line) for line in rig.api.get(f"/api/v2/test/log?since={mark}").data["lines"])
    assert "too elongated: 720x144" in text, "it was loaded, measured and passed over, not skipped by luck"
    try:
        uart.command("settings set", {"aspect_filter": False})
        for _ in range(12):
            status = rig.step("next")
            seen.add(status["current"])
            if "strip" in seen:
                break
        assert "strip" in seen, "with the filter off it plays like any other"
        assert rig.on_panel()[:2] == (720, 144)
    finally:
        uart.command("settings set", {"aspect_filter": True})
