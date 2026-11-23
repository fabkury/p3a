# SPDX-License-Identifier: Apache-2.0
"""The loader pipeline on the device: moving on by itself, pause, what a
failure leads to depending on who asked, and showing one file now.

The rules are host-tested (sched_turn, sched_pick); this is the wiring. A
test build holds the device still from boot ("sched auto off") so that
other tests find on the panel what they put there; the tests here that are
about moving on switch it back for their duration.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = pytest.mark.live

GOOD = {
    "zz-pipe-a.gif": "gif/opaque_anim.gif",
    "zz-pipe-b.png": "png/rgb.png",
    "zz-pipe-c.jpg": "jpeg/baseline.jpg",
}
BAD = "zz-pipe-bad.png"
OURS = list(GOOD) + [BAD, "zz-pipe-new.png"]
LOCAL_ONLY = {"name": "", "channels": [{"source": "local"}]}


def player(api) -> dict:
    return api.get("/api/v2/player").data


def wait_player(api, what: str, test, timeout: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout
    state = player(api)
    while not test(state) and time.monotonic() < deadline:
        time.sleep(0.2)
        state = player(api)
    assert test(state), f"{what}: {state}"
    return state


def put(api, name: str, body: bytes) -> None:
    assert api.call("POST", f"/api/v2/test/local?name={name}", body, timeout=30).status == 200


def relist(uart, api, records: int) -> dict:
    assert uart.command("sched refresh 1") == {"result": "ok"}
    return wait_player(api, "the folder listed again",
                       lambda s: s["channels"][0]["state"] == "ready" and s["channels"][0]["records"] == records)


def step(api, verb: str = "next") -> dict:
    before = player(api)
    assert api.call("POST", f"/api/v2/player/{verb}").status == 200
    return wait_player(api, f"{verb} was shown",
                       lambda s: s["counters"]["shown"] > before["counters"]["shown"] and not s["loading"])


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])

    def clean():
        for name in OURS:
            api.call("POST", f"/api/v2/test/local?name={name}")
        uart.command("sched auto off")
        uart.command("sched pause off")
        uart.command("settings set", {"dwell_s": 30, "pick_mode": "random", "brightness": 100})

    clean()
    assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    for name, source in GOOD.items():
        put(api, name, (CORPUS / "good" / source).read_bytes())
    relist(uart, api, 3)
    wait_player(api, "one of them plays", lambda s: s["playing"] and not s["loading"])
    yield uart, api
    clean()
    uart.command("sched activate", LOCAL_ONLY)
    api.call("POST", "/api/v2/test/play/stop")


def test_it_moves_on_by_itself_after_the_dwell_and_never_to_the_same_artwork(rig):
    uart, api = rig
    start = step(api)
    assert "advance_in_s" not in start, "held still, as a test build is from boot"
    uart.command("settings set", {"dwell_s": 2})
    assert uart.command("sched auto on") == {"result": "ok"}
    try:
        seen = [start["current"]["name"]]
        shown = start["counters"]["shown"]
        for _ in range(4):
            state = wait_player(api, "it moved on", lambda s: s["counters"]["shown"] > shown and not s["loading"],
                                timeout=8)
            assert state["counters"]["shown"] == shown + 1, "one step per dwell"
            shown = state["counters"]["shown"]
            assert state["current"]["name"] != seen[-1], "not the same one twice in a row"
            assert 0 <= state.get("advance_in_s", -1) <= 2
            seen.append(state["current"]["name"])
        assert state["counters"]["problems"] == start["counters"]["problems"]

        # A dwell of 0 means it stays.
        uart.command("settings set", {"dwell_s": 0})
        time.sleep(3.0)
        still = player(api)
        assert still["counters"]["shown"] == shown and "advance_in_s" not in still
    finally:
        uart.command("sched auto off")
        uart.command("settings set", {"dwell_s": 30})


def test_pause_darkens_the_panel_and_stops_everything_until_resumed(rig):
    uart, api = rig
    uart.command("settings set", {"brightness": 60, "dwell_s": 2})
    uart.command("sched auto on")
    try:
        before = step(api)
        assert api.call("POST", "/api/v2/player/pause").status == 200
        assert api.call("POST", "/api/v2/player/pause").status == 200, "pausing twice is pausing"
        paused = player(api)
        assert paused["paused"] is True and "advance_in_s" not in paused
        assert uart.command("lcd info")["brightness"] == 0, "the backlight is off"
        api.call("POST", "/api/v2/player/next").error(409, "PAUSED")
        api.call("POST", "/api/v2/player/back").error(409, "PAUSED")
        # A brightness set meanwhile waits for the resume.
        uart.command("settings set", {"brightness": 40})
        time.sleep(3.0)
        assert uart.command("lcd info")["brightness"] == 0
        assert player(api)["counters"]["shown"] == before["counters"]["shown"], "nothing moved on while paused"

        assert api.call("POST", "/api/v2/player/resume").status == 200
        resumed = player(api)
        assert resumed["paused"] is False and resumed["current"] == before["current"]
        assert uart.command("lcd info")["brightness"] == 40
        assert resumed.get("advance_in_s") == 2, "a full dwell after the resume"
        wait_player(api, "it moves on again", lambda s: s["counters"]["shown"] > before["counters"]["shown"], timeout=8)
    finally:
        uart.command("sched pause off")
        uart.command("sched auto off")
        uart.command("settings set", {"brightness": 100, "dwell_s": 30})


def test_next_passes_over_a_damaged_file_without_a_word_and_never_deletes_it(rig):
    uart, api = rig
    put(api, BAD, (CORPUS / "bad" / "png" / "png_truncated_half.png").read_bytes())
    listed = relist(uart, api, 4)
    assert listed["channels"][0]["available"] == 4
    before = player(api)
    for _ in range(8):
        state = step(api)
        assert state["current"]["name"] in GOOD, "every next ends on something that plays"
    assert state["counters"]["failed"] == before["counters"]["failed"] + 1, "the damaged one was tried once"
    assert state["counters"]["problems"] == before["counters"]["problems"] and "problem" not in state
    assert state["channels"][0]["available"] == 3, "and set aside"
    assert relist(uart, api, 4)["channels"][0]["available"] == 4, "it is still on the card"
    api.call("POST", f"/api/v2/test/local?name={BAD}")
    relist(uart, api, 3)


def test_a_named_artwork_that_fails_is_reported_and_nothing_else_is_tried(rig):
    uart, api = rig
    first = step(api)
    second = step(api)
    assert first["current"] != second["current"]
    gone = first["current"]["name"]
    api.call("POST", f"/api/v2/test/local?name={gone}")  # removed behind the scheduler's back
    failed = second["counters"]["failed"]
    assert api.call("POST", "/api/v2/player/back").status == 200
    state = wait_player(api, "back was tried", lambda s: s["counters"]["failed"] == failed + 1 and not s["loading"])
    assert state["problem"] == "MISSING" and state["counters"]["problems"] == second["counters"]["problems"] + 1
    assert state["current"] == second["current"], "what was playing still is"
    assert state["counters"]["shown"] == second["counters"]["shown"], "nothing else was put up in its place"
    # The next thing shown clears it.
    assert "problem" not in step(api)
    put(api, gone, (CORPUS / "good" / GOOD[gone]).read_bytes())
    relist(uart, api, 3)


def test_an_uploaded_file_plays_at_once_and_joins_the_history(rig):
    uart, api = rig
    before = step(api)
    body = (CORPUS / "good" / "png" / "gray.png").read_bytes()
    reply = api.call("POST", "/api/v2/files?name=zz-pipe-new.png", body, timeout=30)
    assert reply.status == 200 and reply.data["playing"] is True
    state = wait_player(api, "it is on the panel",
                        lambda s: s.get("current", {}).get("name") == "zz-pipe-new.png" and not s["loading"])
    assert state["history"]["count"] == before["history"]["count"] + 1 and state["history"]["position"] == 0
    assert api.call("POST", "/api/v2/player/back").status == 200
    back = wait_player(api, "back to what played before", lambda s: s["history"]["position"] == 1 and not s["loading"])
    assert back["current"] == before["current"]

    # While paused an upload is kept and listed, and does not light the panel.
    api.call("POST", "/api/v2/player/pause")
    try:
        reply = api.call("POST", "/api/v2/files?name=zz-pipe-new.png", body, timeout=30)
        assert reply.status == 200 and reply.data["playing"] is False
    finally:
        api.call("POST", "/api/v2/player/resume")
