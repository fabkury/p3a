# SPDX-License-Identifier: Apache-2.0
"""The scheduler on the device: the active playset's channels are listed
through their sources, artworks are picked and handed to the player, the
history serves "back", a failed artwork costs nothing, and the playset is
still active after a restart.

The arithmetic (shares, picks, history) is host-tested; this is the wiring.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = pytest.mark.live

# name on the card -> (corpus file, width, height, frames)
FILES = {
    "zz-sched-a.gif": ("gif/opaque_anim.gif", 36, 28, 4),
    "zz-sched-b.png": ("png/rgb.png", 48, 32, 1),
    "zz-sched-c.jpg": ("jpeg/baseline.jpg", 64, 48, 1),
}
LOCAL_ONLY = {"name": "", "channels": [{"source": "local"}]}


class Rig:
    def __init__(self, uart, api):
        self.uart, self.api = uart, api

    def put(self, name: str, body: bytes) -> None:
        assert self.api.call("POST", f"/api/v2/test/local?name={name}", body, timeout=30).status == 200

    def remove(self, name: str) -> None:
        self.api.call("POST", f"/api/v2/test/local?name={name}")

    def status(self) -> dict:
        return self.uart.command("sched status")

    def wait(self, what: str, test, timeout: float = 15.0) -> dict:
        deadline = time.monotonic() + timeout
        status = self.status()
        while not test(status) and time.monotonic() < deadline:
            time.sleep(0.2)
            status = self.status()
        assert test(status), f"{what}: {status}"
        return status

    def relist(self, records: int) -> dict:
        assert self.uart.command("sched refresh 1") == {"result": "ok"}
        return self.wait("the folder listed again",
                         lambda s: s["channels"][0]["state"] == "ready" and s["channels"][0]["records"] == records)

    def step(self, command: str) -> dict:
        """next or back, and wait for the player's report on it."""
        before = self.status()
        assert self.uart.command(f"sched {command}") == {"result": "ok"}
        return self.wait(f"{command} reported", lambda s: s["shown"] + s["failed"] > before["shown"] + before["failed"]
                         and not s["loading"])

    def on_panel(self) -> tuple:
        data = self.api.get("/api/v2/test/play").data
        return data["width"], data["height"], data["frame_count"]


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    leftovers = list(FILES) + ["zz-sched-bad.png", "check-jpeg.jpg", "check-webp.webp"]
    for name in leftovers:
        r.remove(name)
    uart.command("settings set", {"pick_mode": "recency", "channel_select": "swrr"})
    assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    yield r
    for name in leftovers:
        r.remove(name)
    uart.command("settings set", {"pick_mode": "random", "channel_select": "stochastic"})
    uart.command("sched activate", LOCAL_ONLY)
    r.api.call("POST", "/api/v2/test/play/stop")


def test_the_users_files_are_listed_and_played_in_turn(rig):
    empty = rig.relist(0)
    assert empty["channels"][0]["source"] == "local" and empty["channels"][0]["available"] == 0
    # An earlier test may have left the history part way back: "next" first
    # walks forward to the present, and only then has to pick.
    results = []
    while len(results) < 40 and (not results or results[-1] == "ok"):
        results.append(rig.uart.command("sched next")["result"])
    assert results[-1] == "ESP_ERR_NOT_FOUND", "nothing to play is said, not hidden"
    for name, (source, *_rest) in FILES.items():
        rig.put(name, (CORPUS / "good" / source).read_bytes())
    listed = rig.relist(3)
    assert listed["channels"][0]["available"] == 3
    rig.wait("something of the folder is playing", lambda s: s["playing"] and s["current"] in FILES)

    # In channel order every file comes up once before any comes up again.
    seen = [rig.step("next")["current"] for _ in range(3)]
    assert sorted(seen) == sorted(FILES), seen
    assert rig.step("next")["current"] == seen[0], "and then round again"
    # What the scheduler says is playing is what the player has on the panel.
    now = rig.status()["current"]
    assert rig.on_panel() == tuple(FILES[now][1:])


def test_back_walks_the_history_and_next_walks_forward_again(rig):
    start = rig.status()
    assert start["history"] >= 3 and start["position"] == 0
    present = start["current"]
    one = rig.step("back")
    assert one["position"] == 1 and one["current"] != present
    two = rig.step("back")
    assert two["position"] == 2 and two["history"] == start["history"], "going back adds nothing to the history"
    assert rig.step("next")["current"] == one["current"]
    again = rig.step("next")
    assert again["current"] == present and again["position"] == 0
    assert again["picks"] == start["picks"], "none of that was a new pick"
    assert rig.on_panel() == tuple(FILES[present][1:])


def test_an_artwork_that_fails_costs_nothing_and_leaves_the_last_one_playing(rig):
    before = rig.status()
    playing, panel = before["current"], rig.on_panel()
    # The files vanish behind the scheduler's back (a USB session, say).
    for name in FILES:
        rig.remove(name)
    # One "next" tries the others by itself: all three are found missing.
    assert rig.uart.command("sched next") == {"result": "ok"}
    after = rig.wait("every file was tried", lambda s: s["channels"][0]["available"] == 0 and not s["loading"])
    assert after["failed"] == before["failed"] + 3 and after["last_error"] == "MISSING"
    assert after["problem"] == "MISSING", "a person asked, so it is said"
    assert after["current"] == playing and after["history"] == before["history"], "nothing shown, nothing recorded"
    assert rig.uart.command("sched next")["result"] == "ESP_ERR_NOT_FOUND"
    assert rig.on_panel() == panel, "the last good artwork is still on the panel"

    # A damaged file of the user's: reported, set aside, and left on the card.
    rig.put("zz-sched-bad.png", (CORPUS / "bad" / "png" / "png_truncated_half.png").read_bytes())
    rig.relist(1)
    assert rig.uart.command("sched next") == {"result": "ok"}
    bad = rig.wait("it was tried", lambda s: s["failed"] == after["failed"] + 1 and not s["loading"])
    assert bad["last_error"] == "CORRUPT" and bad["problem"] == "CORRUPT" and bad["current"] == playing
    assert bad["channels"][0]["available"] == 0, "not offered again until the folder is listed anew"
    assert rig.relist(1)["channels"][0]["available"] == 1, "still on the card: a user's file is never deleted"
    rig.remove("zz-sched-bad.png")


def test_the_active_playset_is_resumed_after_a_restart(rig):
    for name, (source, *_rest) in FILES.items():
        rig.put(name, (CORPUS / "good" / source).read_bytes())
    mix = {"name": "Restart mix", "channels": [{"source": "makapix", "kind": "all", "weight": 5},
                                               {"source": "local", "weight": 1}]}
    assert rig.uart.command("sched activate", mix) == {"result": "ok"}
    status = rig.wait("the new playset is playing",
                      lambda s: s["playset"] == "Restart mix" and s["channels"][1]["state"] == "ready" and s["playing"]
                      and not s["loading"])
    assert status["channels"][0]["state"] == "no_source", "a source this firmware lacks is passed over, not an error"
    assert status["channels"][0]["source"] == "makapix" and status["current"] in FILES

    start = rig.uart.mark()
    rig.uart.command("reboot")
    rig.uart.wait_ready(since=start, timeout=30)
    resumed = rig.wait("playing again after the restart",
                       lambda s: s["playing"] and not s["loading"], timeout=20)
    assert resumed["playset"] == "Restart mix" and len(resumed["channels"]) == 2
    assert resumed["current"] in FILES and resumed["shown"] == 1, "one artwork, chosen by itself at boot"
