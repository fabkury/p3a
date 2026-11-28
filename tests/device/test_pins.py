# SPDX-License-Identifier: Apache-2.0
"""Pin lists on the device: an artwork on the screen is pinned and unpinned,
a list plays as a channel and follows the list, a pinned artwork's bytes
survive eviction, and the lists are still there after a restart.

The table of lists and the set of kept blobs are host-tested
(test_pin_table.c). This is the files, the API and the scheduler's side.
The artworks come from the test hooks' source, as in test_content.py.
"""
import hashlib
import time
import urllib.request

import pytest

import lab
from test_api import Api
from test_content import GALLERY, PICTURES, WEBP, items
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live

RUN = int(time.time())
REMOTE = {"name": "Pinning", "channels": [{"source": "test", "kind": f"pins{RUN}"}]}
ALL, NOTHING = 10**12, "-"


def channel(status):
    return status["channels"][0]


class Pins:
    def __init__(self, api):
        self.api = api

    def lists(self) -> list:
        reply = self.api.get("/api/v2/pins")
        assert reply.status == 200
        return reply.data["lists"]

    def active(self) -> dict:
        active = [entry for entry in self.lists() if entry["active"]]
        assert len(active) == 1, "exactly one list is the active one"
        return active[0]

    def pins(self, slug: str) -> list:
        reply = self.api.get(f"/api/v2/pins/{slug}")
        assert reply.status == 200
        return reply.data["pins"]


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    r.pins = Pins(r.api)
    r.refs = {}
    for key, name in PICTURES.items():
        with urllib.request.urlopen(GALLERY + name, timeout=30) as reply:
            r.refs[key] = hashlib.sha256(reply.read()).hexdigest()[:32]
    before = r.pins.active()["slug"]
    # A list of this run's own, so that what earlier runs pinned is not in the way.
    made = r.api.call("POST", "/api/v2/pins", {"name": f"Run {RUN}"})
    assert made.status == 200
    r.slug = made.data["slug"]
    assert r.api.call("PUT", f"/api/v2/pins/{r.slug}", {"active": True}).status == 200
    uart.command("settings set", {"dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    r.api.call("PUT", f"/api/v2/pins/{before}", {"active": True})
    r.api.call("DELETE", f"/api/v2/pins/{r.slug}")
    for ref in r.refs.values():
        uart.command(f"library blob-remove {ref} {WEBP}")
    uart.command("settings set", {"dwell_s": 30})


def has(rig, key) -> bool:
    return rig.uart.command(f"library blob-has {rig.refs[key]} {WEBP}")["has"]


def test_there_is_always_a_list_and_one_of_them_is_active(rig):
    lists = rig.pins.lists()
    assert any(entry["name"] == "My Pins" for entry in lists), "made by the firmware when there was none"
    mine = rig.pins.active()
    assert mine["slug"] == rig.slug and mine["name"] == f"Run {RUN}" and mine["count"] == 0
    assert rig.pins.pins("active") == [] and rig.pins.pins(rig.slug) == []


def test_lists_are_made_renamed_and_deleted_but_never_the_active_one(rig):
    api = rig.api
    made = api.call("POST", "/api/v2/pins", {"name": "Café evenings"})
    assert made.status == 200
    slug = made.data["slug"]
    assert len(slug) == 8 and slug != rig.slug
    assert api.call("PUT", f"/api/v2/pins/{slug}", {"name": "Evenings"}).status == 200
    assert {"slug": slug, "name": "Evenings", "active": False, "count": 0} in rig.pins.lists()

    for bad in ("", "x" * 65, "two\nlines"):
        api.call("POST", "/api/v2/pins", {"name": bad}).error(400, "INVALID_NAME")
        api.call("PUT", f"/api/v2/pins/{slug}", {"name": bad}).error(400, "INVALID_NAME")
    for unknown in ("0badf00d", "nonsense", "ACTIVE"):
        assert api.get(f"/api/v2/pins/{unknown}").status == 404
        assert api.call("DELETE", f"/api/v2/pins/{unknown}").status == 404
        assert api.call("PUT", f"/api/v2/pins/{unknown}", {"name": "x"}).status == 404

    refused = api.call("DELETE", f"/api/v2/pins/{rig.slug}")
    refused.error(409, "LIST_IS_ACTIVE")
    assert api.call("DELETE", "/api/v2/pins/active").status == 409
    assert api.call("PUT", f"/api/v2/pins/{slug}", {"active": True}).status == 200
    assert rig.pins.active()["slug"] == slug
    assert api.call("PUT", f"/api/v2/pins/{rig.slug}", {"active": True}).status == 200
    assert api.call("DELETE", f"/api/v2/pins/{slug}").status == 200
    assert slug not in [entry["slug"] for entry in rig.pins.lists()]


def test_the_artwork_on_the_screen_is_pinned_once_and_a_file_of_ones_own_is_not(rig):
    from test_decoders import CORPUS
    api = rig.api
    rig.put("zz-pins-local.png", (CORPUS / "good" / "png/rgb.png").read_bytes())
    try:
        assert rig.uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
        rig.uart.command("sched refresh 1")
        rig.wait("a file of the user's own plays", lambda s: s["playing"] and not s["loading"]
                 and s["current_source"] == "local")
        refused = api.call("POST", "/api/v2/pins/active/pin")
        refused.error(409, "CANNOT_BE_PINNED")
    finally:
        rig.remove("zz-pins-local.png")

    assert rig.uart.command("content set", {"items": items("a", "b", "c"), "result": "ok"}) == {"items": 3}
    assert rig.uart.command("sched activate", REMOTE) == {"result": "ok"}
    status = rig.wait("three artworks downloaded and one playing",
                      lambda s: s["playset"] == "Pinning" and channel(s)["available"] == 3 and s["playing"]
                      and not s["loading"] and not s["downloading"], timeout=120)
    first = status["current"]
    pinned = api.call("POST", "/api/v2/pins/active/pin")
    assert pinned.status == 200 and pinned.data["id"] == first and pinned.data["already"] is False
    again = api.call("POST", f"/api/v2/pins/{rig.slug}/pin")
    assert again.status == 200 and again.data["already"] is True
    entries = rig.pins.pins("active")
    assert len(entries) == 1 and entries[0]["id"] == first and entries[0]["source"] == "giphy"
    assert entries[0]["position"] == 0 and entries[0]["format"] == "webp"

    # A second one, so that the list has an order.
    second = rig.step("next")["current"]
    while second == first:
        second = rig.step("next")["current"]
    assert api.call("POST", "/api/v2/pins/active/pin").data["already"] is False
    assert [entry["id"] for entry in rig.pins.pins("active")] == [first, second], "in the order pinned"
    assert rig.pins.active()["count"] == 2
    rig.first, rig.second = first, second


UP = [[1, 360, 500 - 30 * i, 0, 0, 30] for i in range(9)] + [[0, 0, 0, 0, 0, 60]]
DOWN = [[1, 360, 260 + 30 * i, 0, 0, 30] for i in range(9)] + [[0, 0, 0, 0, 0, 60]]


def test_a_swipe_down_unpins_what_is_on_the_screen_and_a_swipe_up_pins_it(rig):
    def wait_for(ids):
        deadline = time.monotonic() + 10
        while [entry["id"] for entry in rig.pins.pins("active")] != ids and time.monotonic() < deadline:
            time.sleep(0.3)
        assert [entry["id"] for entry in rig.pins.pins("active")] == ids

    assert rig.status()["current"] == rig.second
    assert rig.uart.command("ui touch", DOWN) == {"result": "ok"}
    wait_for([rig.first])
    assert rig.uart.command("ui touched")["last_action"] == "unlike"
    assert rig.uart.command("ui touch", DOWN) == {"result": "ok"}  # not pinned now: nothing to undo, nothing changes
    time.sleep(1.5)
    wait_for([rig.first])
    assert rig.uart.command("ui touch", UP) == {"result": "ok"}
    wait_for([rig.first, rig.second])
    assert rig.uart.command("ui touch", UP) == {"result": "ok"}  # pinned already: once is once
    time.sleep(1.5)
    wait_for([rig.first, rig.second])


def test_a_list_plays_as_a_channel_and_follows_the_list(rig):
    api = rig.api
    listed = {"name": "My pins", "channels": [{"source": "pinned", "identifier": rig.slug}]}
    assert rig.uart.command("sched activate", listed) == {"result": "ok"}
    status = rig.wait("the list plays", lambda s: s["playset"] == "My pins" and channel(s)["state"] == "ready"
                      and s["playing"] and not s["loading"])
    ch = channel(status)
    assert ch["source"] == "pinned" and ch["records"] == 2 and ch["available"] == 2
    assert status["current"] in (rig.first, rig.second)
    assert status["current_source"] == "giphy", "a pinned artwork is still its own source's"
    seen = {status["current"], rig.step("next")["current"]}
    assert seen == {rig.first, rig.second}

    # Unpinning what is on the screen: the channel is listed again by itself.
    gone = rig.status()["current"]
    assert api.call("POST", "/api/v2/pins/active/unpin", {}).status == 200
    status = rig.wait("the channel follows the list", lambda s: channel(s)["state"] == "ready"
                      and channel(s)["records"] == 1)
    kept = rig.first if gone == rig.second else rig.second
    assert [entry["id"] for entry in rig.pins.pins("active")] == [kept]
    refused = api.call("POST", "/api/v2/pins/active/unpin", {"position": 0, "key": 1})
    refused.error(409, "LIST_CHANGED")
    status = rig.step("next")
    assert status["current"] == kept
    rig.kept, rig.gone = kept, gone

    # The active list by its other name, and one that does not exist.
    assert rig.uart.command("sched activate", {"name": "", "channels": [{"source": "pinned"}]}) == {"result": "ok"}
    rig.wait("the active list plays", lambda s: channel(s)["state"] == "ready" and channel(s)["records"] == 1
             and s["playing"] and not s["loading"])
    assert rig.uart.command("sched activate", {"name": "", "channels": [{"source": "pinned", "identifier": "0badf00d"}]}) \
        == {"result": "ok"}
    status = rig.wait("a list that does not exist is a channel that failed", lambda s: channel(s)["state"] == "failed")
    assert channel(status)["error"] == "MISSING"


def key_of(rig, artwork_id: str) -> str:
    return next(key for key in PICTURES if key == artwork_id)


def test_eviction_never_takes_a_pinned_artwork(rig):
    uart = rig.uart
    assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    assert all(has(rig, key) for key in PICTURES)
    done = uart.command(f"library evict {ALL} 0 {NOTHING}", timeout=120)
    assert done["result"] == "ok" and done["deleted"] >= 2
    kept = key_of(rig, rig.kept)
    assert has(rig, kept), "everything was asked for, and the pin stayed"
    assert [key for key in PICTURES if has(rig, key)] == [kept]


def test_the_lists_and_what_they_keep_are_there_after_a_restart(rig):
    uart = rig.uart
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    lab.wait_online(uart)
    # Nothing is evicted before the clock is set, and the network is up some seconds before it is:
    # without this wait both evictions below can do nothing, and the first check passes for no reason.
    deadline = time.monotonic() + 60
    while not uart.command("clock")["set"] and time.monotonic() < deadline:
        time.sleep(0.5)
    assert uart.command("clock")["set"], "the clock should be set within a minute of the network"
    assert rig.pins.active()["slug"] == rig.slug and rig.pins.active()["count"] == 1
    assert [entry["id"] for entry in rig.pins.pins(rig.slug)] == [rig.kept]
    kept = key_of(rig, rig.kept)
    uart.command(f"library evict {ALL} 0 {NOTHING}", timeout=120)
    assert has(rig, kept), "kept after a restart too: the lists are read at boot"

    # Unpinned by its position and key, it is an artwork like any other again.
    entry = rig.pins.pins("active")[0]
    assert rig.api.call("POST", "/api/v2/pins/active/unpin", {"position": 0, "key": entry["key"]}).status == 200
    assert rig.api.call("POST", "/api/v2/pins/active/unpin", {"position": 0, "key": entry["key"]}).status == 409
    assert rig.pins.active()["count"] == 0
    assert uart.command(f"library evict {ALL} 0 {NOTHING}", timeout=120)["deleted"] == 1
    assert not has(rig, kept)


# --- "show this" ------------------------------------------------------------------

def job_done(api, job: int, timeout: float = 60.0) -> dict:
    deadline = time.monotonic() + timeout
    info = api.get(f"/api/v2/jobs/{job}").data
    while info["state"] != "done" and time.monotonic() < deadline:
        time.sleep(0.3)
        info = api.get(f"/api/v2/jobs/{job}").data
    assert info["state"] == "done", info
    return info


def show(api, url: str) -> dict:
    reply = api.call("POST", "/api/v2/player/show", {"url": url})
    assert reply.status == 202, reply.body
    return job_done(api, reply.data["job"])


def test_a_picture_at_an_address_is_fetched_kept_and_played(rig):
    api = rig.api
    rig.remove("1.webp")
    assert rig.uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    try:
        before = rig.status()
        assert show(api, GALLERY + "1.webp?shown=by-test")["succeeded"] is True
        status = rig.wait("it plays", lambda s: s["shown"] > before["shown"] and not s["loading"]
                          and s["current"] == "1.webp", timeout=15)
        assert status["current_source"] == "local", "kept among the user's own files, as an upload is"
        assert rig.on_panel()[2] == 1
        rig.wait("and is in the local channel", lambda s: channel(s)["state"] == "ready" and channel(s)["records"] >= 1)

        # What is not a picture never reaches the user's folder.
        records = channel(rig.status())["records"]
        failed = show(api, "https://example.com/not-a-picture.png")
        assert failed["succeeded"] is False and failed["error"] in ("NOT_PLAYABLE", "MISSING"), failed
        failed = show(api, GALLERY + "no-such-p3a-file.webp")
        assert failed["succeeded"] is False and failed["error"] == "MISSING", failed
        time.sleep(1.0)
        assert channel(rig.status())["records"] == records and rig.status()["current"] == "1.webp"

        for bad in ("", "ftp://example.org/a.gif", "example.org/a.gif", "https://example.org/a b.gif", 7):
            api.call("POST", "/api/v2/player/show", {"url": bad}).error(400, "INVALID_URL")
        assert api.call("POST", "/api/v2/player/pause").status == 200
        try:
            api.call("POST", "/api/v2/player/show", {"url": GALLERY + "2.webp"}).error(409, "PAUSED")
        finally:
            assert api.call("POST", "/api/v2/player/resume").status == 200
    finally:
        rig.remove("1.webp")
        rig.uart.command("sched refresh 1")
