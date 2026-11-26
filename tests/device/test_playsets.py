# SPDX-License-Identifier: Apache-2.0
"""Playsets on the SD card: saved under a name, listed, replaced, deleted,
kept across a restart, and refused rather than overwritten when damaged.

What a playset may contain is host-tested (tests/host/player/test_playset.c).
"""
import time

import pytest

import lab
from test_api import Api

MIX = {"name": "Evening mix", "channels": [
    {"source": "makapix", "kind": "promoted", "weight": 3},
    {"source": "giphy", "kind": "search", "identifier": "pixel cats", "display_name": "Cats", "weight": 1},
    {"source": "local"},
]}


def stripped(playset: dict) -> dict:
    """A playset as the device answers it, without the ids and defaults it adds."""
    channels = [{k: v for k, v in ch.items() if k != "id" and v not in ("", 0)} for ch in playset["channels"]]
    return {"name": playset["name"], "channels": channels}


@pytest.fixture
def clean(uart):
    def wipe():
        for name in uart.command("playset list")["playsets"]:
            uart.command("playset delete", {"name": name})
    wipe()
    yield uart
    wipe()


def test_a_playset_is_saved_listed_replaced_and_deleted(clean):
    uart = clean
    assert uart.command("playset load", {"name": "Evening mix"})["result"] == "MISSING"
    assert uart.command("playset save", MIX) == {"result": "ok"}
    got = uart.command("playset load", {"name": "Evening mix"})
    assert got["result"] == "ok" and stripped(got["playset"]) == MIX
    assert all(len(ch["id"]) == 16 for ch in got["playset"]["channels"])

    smaller = {"name": "Evening mix", "channels": [{"source": "local", "weight": 5}]}
    other = {"name": "evening mix", "channels": [{"source": "klipy", "kind": "trending"}]}  # differs by case only
    assert uart.command("playset save", smaller) == {"result": "ok"}
    assert uart.command("playset save", other) == {"result": "ok"}
    listed = uart.command("playset list")
    assert listed["playsets"] == {"Evening mix": 1, "evening mix": 1} and listed["unreadable"] == 0

    assert uart.command("playset delete", {"name": "Evening mix"}) == {"result": "ok"}
    assert uart.command("playset delete", {"name": "Evening mix"})["result"] == "MISSING"
    assert uart.command("playset list")["playsets"] == {"evening mix": 1}


def test_what_is_not_a_playset_is_not_saved(clean):
    uart = clean
    refused = uart.command("playset save", {"name": "x", "channels": [{"source": "local", "wieght": 2}]})
    assert refused["result"] == "ESP_ERR_INVALID_ARG" and "wieght" in refused["why"]
    assert uart.command("playset save", {"channels": [{"source": "local"}]})["result"] == "ESP_ERR_INVALID_ARG"
    assert uart.command("playset list")["playsets"] == {}


def test_a_damaged_playset_is_reported_and_not_overwritten(clean):
    uart = clean
    assert uart.command("playset save", MIX) == {"result": "ok"}
    assert uart.command("playset damage", {"name": "Evening mix"}) == {"result": "ok"}
    assert uart.command("playset load", {"name": "Evening mix"})["result"] == "CORRUPT"
    listed = uart.command("playset list")
    assert listed["playsets"] == {} and listed["unreadable"] == 1, "counted, not hidden and not removed"
    assert uart.command("playset save", MIX)["result"] == "ESP_ERR_INVALID_STATE", "the user's file is not written over"
    assert uart.command("playset list")["unreadable"] == 1
    # Only the user's own delete removes it.
    assert uart.command("playset delete", {"name": "Evening mix"}) == {"result": "ok"}
    assert uart.command("playset save", MIX) == {"result": "ok"}


def test_no_more_than_32_playsets_are_saved(clean):
    uart = clean
    for n in range(32):
        assert uart.command("playset save", {"name": f"set {n}", "channels": [{"source": "local"}]}) == {"result": "ok"}
    assert uart.command("playset save", {"name": "one more", "channels": [{"source": "local"}]})["result"] == "ESP_ERR_NO_MEM"
    # Replacing one of the 32 is not one more.
    assert uart.command("playset save", {"name": "set 7", "channels": [{"source": "giphy"}]}) == {"result": "ok"}
    assert len(uart.command("playset list")["playsets"]) == 32


def test_the_active_playset_survives_a_restart(clean):
    uart = clean
    unnamed = {"name": "", "channels": [{"source": "makapix", "kind": "cma:department", "identifier": "Japanese Art",
                                         "offset": 200}]}
    assert uart.command("playset active-save", unnamed) == {"result": "ok"}
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    got = uart.command("playset active-load")
    assert got["result"] == "ok" and stripped(got["playset"]) == unnamed
    assert uart.command("playset list")["playsets"] == {}, "the active playset is not one of the saved ones"


@pytest.mark.live
def test_a_device_never_told_what_to_play_plays_its_card_and_makapix_promoted(clean):
    uart = clean
    lab.provision(uart)
    try:
        assert uart.command("playset active-forget")["result"] == "ok"
        start = uart.mark()
        uart.command("reboot")
        uart.wait_ready(since=start, timeout=40)
        channels = uart.command("sched status")["channels"]
        assert [c["source"] for c in channels] == ["local", "makapix"]
        # Promoted needs no account and no key (D90): on Wi-Fi it is listed from the site's public feed.
        api = Api(lab.wait_online(uart, timeout=60)["ip"])
        assert [(c["source"], c["kind"]) for c in api.get("/api/v2/player").data["channels"]] == \
            [("local", ""), ("makapix", "promoted")]
        deadline = time.monotonic() + 90
        promoted = channels[1]
        while promoted["records"] == 0 and time.monotonic() < deadline:
            time.sleep(2)
            promoted = uart.command("sched status")["channels"][1]
        assert promoted["records"] > 0 and "error" not in promoted, promoted
        # The default is not written down: told nothing, the device stays one that was told nothing.
        assert uart.command("playset active-load")["result"] != "ok"
    finally:
        uart.command("sched activate", {"name": "", "channels": [{"source": "local"}]})
