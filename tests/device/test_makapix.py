# SPDX-License-Identifier: Apache-2.0
"""Makapix Club on the device, against the production service (D13).

Without a registration: the promoted channel is listed from the public
feed, its artworks fetched from the vault and played; every other channel
says that it needs a registration.

What the answers mean is host-tested on recorded pages
(test_makapix_parse.c). This is the requests really made, the vault's
addresses really answering, and the pictures really playing.
"""
import time

import pytest

import lab
from test_api import Api
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live


def playset(kind, identifier=""):
    return {"name": "Makapix", "channels": [{"source": "makapix", "kind": kind, "identifier": identifier}]}


def channel(status):
    return status["channels"][0]


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    uart.command("settings set", {"makapix_cache_size": 32, "dwell_s": 600})
    # The test build holds the player still from boot with the hold a covering screen uses, and
    # an artwork under a cover is not "seen". Released here; nothing moves on in ten minutes.
    uart.command("sched auto on")
    yield r
    uart.command("sched auto off")
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"makapix_cache_size": 2048, "dwell_s": 30})


def registered(rig) -> bool:
    return rig.api.get("/api/v2/makapix").data["registered"]


def test_the_promoted_channel_plays_from_the_public_feed(rig):
    if registered(rig):
        pytest.skip("this unit is registered: the public feed is what an unregistered one uses")
    before = rig.status()
    assert rig.uart.command("sched activate", playset("promoted")) == {"result": "ok"}
    status = rig.wait("the channel listed", lambda s: s["playset"] == "Makapix" and channel(s)["state"] == "ready"
                      and channel(s)["records"] > 0, timeout=60)
    ch = channel(status)
    assert ch["source"] == "makapix" and ch["records"] == 32, "as many as the setting asks for, from one page"
    # A channel this unit listed in an earlier run is played from its index on the card, and
    # listed again when that is four hours old.
    assert 0 < ch["refresh_in_s"] <= 14400, "listed again within four hours"
    status = rig.wait("artworks downloaded and one playing",
                      lambda s: channel(s)["available"] >= 3 and s["playing"] and not s["loading"], timeout=120)
    assert status["current_source"] == "makapix"
    assert status["download_failures"] == before["download_failures"], "the vault has every file the feed names"
    width, height, frames = rig.on_panel()
    assert 8 <= width <= 256 and 8 <= height <= 256 and frames >= 1, "pixel art, at most 256 each way"
    shown = rig.api.get("/api/v2/player").data["current"]
    assert shown["source"] == "makapix" and shown["post_id"] > 0 and shown["format"] in ("webp", "gif", "png", "bmp")
    status = rig.wait("all of them downloaded", lambda s: channel(s)["available"] == 32 and not s["downloading"],
                      timeout=240)
    assert status["download_failures"] == before["download_failures"]


def test_the_other_channels_need_a_registration(rig):
    if registered(rig):
        pytest.skip("this unit is registered")
    for kind, identifier in (("all", ""), ("user", ""), ("by_user", "QVJ"), ("hashtag", "pixelart"),
                             ("reactions", "QVJ")):
        assert rig.uart.command("sched activate", playset(kind, identifier)) == {"result": "ok"}
        status = rig.wait(f"{kind} refused", lambda s: channel(s)["state"] == "failed", timeout=15)
        assert channel(status)["error"] == "NO_KEY", kind


def test_a_kind_makapix_does_not_have_is_refused_without_asking(rig):
    for kind, identifier in (("artwork", ""), ("by_user", ""), ("hashtag", "#art"), ("hashtag", 'a","x":"y')):
        assert rig.uart.command("sched activate", playset(kind, identifier)) == {"result": "ok"}
        status = rig.wait(f"{kind} refused", lambda s: channel(s)["state"] == "failed", timeout=15)
        assert channel(status)["error"] == "ESP_ERR_NOT_SUPPORTED", (kind, identifier)


def makapix(rig) -> dict:
    return rig.api.get("/api/v2/makapix").data


def wait_registration(rig, what, test, timeout=30.0) -> dict:
    deadline = time.monotonic() + timeout
    state = makapix(rig)
    while not test(state["registration"]) and time.monotonic() < deadline:
        time.sleep(0.3)
        state = makapix(rig)
    assert test(state["registration"]), f"{what}: {state}"
    return state


def test_a_registration_shows_a_code_and_a_cancel_leaves_the_device_as_it_was(rig):
    """One code is asked of the production service (it allows ten an hour from one address)
    and the registration is cancelled: nobody types the code."""
    was = makapix(rig)
    assert was["registration"]["phase"] == "idle"
    try:
        assert rig.api.call("POST", "/api/v2/makapix/registration").status == 200
        state = wait_registration(rig, "a code or a refusal",
                                  lambda r: r["phase"] == "showing_code" or r.get("last_result", "").startswith("no code"))
        if state["registration"]["phase"] != "showing_code":
            assert "RATE_LIMITED" in state["registration"]["last_result"], state
            pytest.skip("the service is not giving this address codes just now (ten an hour)")
        registration = state["registration"]
        code = registration["code"]
        assert len(code) == 6 and code.isalnum() and code == code.upper()
        assert 60 <= registration["seconds_left"] <= 900
        ui = rig.api.get("/api/v2/ui").data
        assert ui["screen"] == "registration" and ui["title"] == code, "the code is what the screen says, large"
        assert state["registered"] == was["registered"], "nothing is saved while the code waits"

        # A long press on the screen means "not now": the screen goes and the code with it.
        rig.uart.command("ui touch", [[1, 360, 200, 0, 0, 2400], [0, 0, 0, 0, 0, 100]])
        state = wait_registration(rig, "given up by a long press", lambda r: r["phase"] == "idle")
        assert state["registration"]["last_result"] == "cancelled"
        assert rig.api.get("/api/v2/ui").data["screen"] != "registration"

        # Starting again asks for a new code.
        assert rig.api.call("POST", "/api/v2/makapix/registration").status == 200
        state = wait_registration(rig, "another code", lambda r: r["phase"] == "showing_code" and r["code"] != code
                                  or r.get("last_result", "").startswith("no code"))
    finally:
        assert rig.api.call("DELETE", "/api/v2/makapix/registration").status in (200, 409)
    state = wait_registration(rig, "cancelled", lambda r: r["phase"] == "idle")
    assert state["registration"]["last_result"] in ("cancelled", "no code: RATE_LIMITED")
    assert state["registered"] == was["registered"]
    deadline = time.monotonic() + 15
    while rig.api.get("/api/v2/ui").data["screen"] == "registration" and time.monotonic() < deadline:
        time.sleep(0.3)
    assert rig.api.get("/api/v2/ui").data["screen"] != "registration"
    if not was["registered"]:
        rig.api.call("DELETE", "/api/v2/makapix/registration").error(409, "NOT_REGISTERED")


# --- what the link is told, and what a command does ---------------------------------
#
# The unit need not be registered for these: the scheduler tells the source what is seen
# whatever the link's state, and the test hooks hand the source a command as the link would.

def command(rig, command_type, payload=None, command_id="test") -> None:
    message = {"command_id": command_id, "command_type": command_type, "payload": payload or {}}
    assert rig.uart.command("makapix command", message) == {"result": "ok"}


def player(rig) -> dict:
    return rig.api.get("/api/v2/player").data


def wait_player(rig, what, test, timeout=20.0) -> dict:
    deadline = time.monotonic() + timeout
    state = player(rig)
    while not test(state) and time.monotonic() < deadline:
        time.sleep(0.3)
        state = player(rig)
    assert test(state), f"{what}: {state}"
    return state


def wait_makapix(rig, what, test, timeout=10.0) -> dict:
    deadline = time.monotonic() + timeout
    state = makapix(rig)
    while not test(state) and time.monotonic() < deadline:
        time.sleep(0.3)
        state = makapix(rig)
    assert test(state), f"{what}: {state}"
    return state


def play_promoted(rig) -> dict:
    assert rig.uart.command("sched activate", playset("promoted")) == {"result": "ok"}
    rig.wait("a Makapix artwork plays", lambda s: s["playset"] == "Makapix" and s["playing"] and not s["loading"]
             and s["current_source"] == "makapix", timeout=120)
    return player(rig)


def test_the_source_knows_which_of_its_artworks_is_seen(rig):
    shown = play_promoted(rig)["current"]["post_id"]
    wait_makapix(rig, "that post is what a view would be for", lambda m: m.get("seen_post_id") == shown)
    # Paused, nothing is seen; resumed, the same post is.
    assert rig.api.call("POST", "/api/v2/player/pause").status == 200
    try:
        wait_makapix(rig, "nothing is seen while paused", lambda m: "seen_post_id" not in m)
    finally:
        assert rig.api.call("POST", "/api/v2/player/resume").status == 200
    wait_makapix(rig, "seen again", lambda m: m.get("seen_post_id") == shown)
    # Another artwork: the link follows.
    rig.step("next")
    state = player(rig)
    wait_makapix(rig, "the next post", lambda m: m.get("seen_post_id") == state["current"]["post_id"])
    # A file of the user's own on the panel: nothing of Makapix's is seen.
    from test_decoders import CORPUS
    rig.put("zz-makapix-local.png", (CORPUS / "good" / "png/rgb.png").read_bytes())
    try:
        assert rig.uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
        rig.uart.command("sched refresh 1")
        rig.wait("a local file plays", lambda s: s["playing"] and not s["loading"] and s["current_source"] == "local")
        wait_makapix(rig, "nothing of Makapix's", lambda m: "seen_post_id" not in m)
    finally:
        rig.remove("zz-makapix-local.png")


def test_the_owners_commands_are_the_players_own_calls(rig):
    uart = rig.uart
    before = play_promoted(rig)
    # Next and back.
    command(rig, "swap_next")
    after = wait_player(rig, "next was played", lambda p: p["counters"]["shown"] > before["counters"]["shown"]
                        and not p["loading"])
    command(rig, "swap_back")
    wait_player(rig, "back was played", lambda p: p["counters"]["shown"] > after["counters"]["shown"] and not p["loading"]
                and p["current"] == before["current"])
    # The settings, and pause.
    want = {"brightness": 40, "rotation": 90, "background_color": "#102030"}
    try:
        command(rig, "set_brightness", {"value": 40})
        command(rig, "set_rotation", {"value": 90})
        command(rig, "set_background_color", {"r": 16, "g": 32, "b": 48})
        deadline = time.monotonic() + 10
        got = {}
        while got != want and time.monotonic() < deadline:
            time.sleep(0.3)
            settings = uart.command("settings get")
            got = {key: settings[key] for key in want}
        assert got == want
        # What will not do changes nothing.
        command(rig, "set_rotation", {"value": 45})
        command(rig, "set_brightness", {"value": "bright"})
        command(rig, "set_mirror", {"value": "horizontal"})
        command(rig, "a_command_from_the_future", {"x": 1})
        assert uart.command("makapix command", {"command_type": 7}) == {"result": "ok"}  # taken, read, dropped
        time.sleep(2.5)
        settings = uart.command("settings get")
        assert {key: settings[key] for key in want} == want
        command(rig, "set_paused", {"paused": True})
        wait_player(rig, "paused", lambda p: p["paused"])
        command(rig, "set_paused", {"paused": False})
        wait_player(rig, "resumed", lambda p: not p["paused"])
    finally:
        uart.command("settings set", {"brightness": 100, "rotation": 0, "background_color": "#000000"})
        rig.api.call("POST", "/api/v2/player/resume")
    # A channel to play.
    command(rig, "play_channel", {"channel_name": "sdcard"})
    state = wait_player(rig, "the local channel is the playset", lambda p: len(p["channels"]) == 1
                        and p["channels"][0]["source"] == "local")
    assert state["playset"] == ""
    command(rig, "play_channel", {"channel_name": "hashtag"})  # no tag: refused
    time.sleep(2.0)
    assert player(rig)["channels"][0]["source"] == "local"
    command(rig, "play_playset", {"playset_name": "followed_artists", "channels": [
        {"type": "named", "name": "promoted", "display_name": "Picks"}, {"type": "sdcard", "weight": 2}]})
    state = wait_player(rig, "the playset the server sent", lambda p: p["playset"] == "followed_artists")
    assert [c["source"] for c in state["channels"]] == ["makapix", "local"]
    saved = [p["name"] for p in rig.api.get("/api/v2/playsets").data["playsets"]]
    assert "followed_artists" not in saved, "played, not kept: the server's name says what it is"


def test_a_post_the_owner_sends_is_fetched_and_shown(rig):
    import json
    import urllib.request
    # A post the device's channel does not hold (it keeps the first 32 of the feed).
    request = urllib.request.Request(
        "https://makapix.club/api/v1/feed/promoted?limit=40&fields=id,storage_key,art_url,width,height",
        headers={"User-Agent": "p3a-test"})
    with urllib.request.urlopen(request, timeout=30) as reply:
        feed = json.loads(reply.read())["items"]
    assert len(feed) == 40
    post = feed[-1]
    play_promoted(rig)
    before = rig.status()
    command(rig, "show_artwork", {"post_id": post["id"], "title": "sent by the test", "storage_key": post["storage_key"],
                                  "storage_shard": "", "native_format": post["art_url"].rsplit(".", 1)[1],
                                  "width": post["width"], "height": post["height"]})
    state = wait_player(rig, "the post is on the panel", lambda p: not p["loading"]
                        and p["current"].get("post_id") == post["id"], timeout=60)
    assert state["current"]["source"] == "makapix"
    assert rig.status()["download_failures"] == before["download_failures"]
    wait_makapix(rig, "and it is what a view would be for", lambda m: m.get("seen_post_id") == post["id"])
    width, height, _ = rig.on_panel()
    assert (width, height) == (post["width"], post["height"])
    # Another post, named by its address.
    other = feed[-2]
    command(rig, "swap_to", {"channel": "promoted", "post_id": other["id"], "storage_key": other["storage_key"],
                             "art_url": other["art_url"]})
    wait_player(rig, "the other post", lambda p: not p["loading"] and p["current"].get("post_id") == other["id"], timeout=60)
    # A post that does not exist: nothing changes.
    command(rig, "show_artwork", {"post_id": 999999999, "storage_key": "00000000-0000-4000-8000-000000000000",
                                  "native_format": "webp"})
    time.sleep(8.0)
    assert player(rig)["current"].get("post_id") == other["id"]
    # An address.
    rig.remove("2.webp")
    try:
        command(rig, "show_url", {"artwork_url": "https://www.gstatic.com/webp/gallery/2.webp"})
        wait_player(rig, "the picture at the address", lambda p: not p["loading"] and p["current"].get("name") == "2.webp",
                    timeout=60)
    finally:
        rig.remove("2.webp")
        rig.uart.command("sched refresh 1")
