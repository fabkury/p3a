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
    yield r
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

        # Starting again ends this one and asks for a new code.
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
