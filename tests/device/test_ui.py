# SPDX-License-Identifier: Apache-2.0
"""The screen model on the device: what is on the screen follows from what
is up, the notices about playback follow the scheduler, and a screen over
the artwork holds playback.

The model is host-tested (ui_model, ui_player); this is the wiring. Nothing
is drawn yet (M3.2, M3.3): the tests read the model through GET /api/v2/ui.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = pytest.mark.live

# p3a_screen_t and p3a_notice_t (p3a_ui_hooks.h)
INFO, SETUP, UPDATE = 3, 6, 8
LOADING, SOURCE_PROBLEM, CARD_REPAIRED = 0, 3, 5
FILES = {"zz-ui-a.gif": "gif/opaque_anim.gif", "zz-ui-b.png": "png/rgb.png"}
LOCAL_ONLY = {"name": "", "channels": [{"source": "local"}]}


def ui(api) -> dict:
    return api.get("/api/v2/ui").data


def wait_ui(api, what: str, test, timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    state = ui(api)
    while not test(state) and time.monotonic() < deadline:
        time.sleep(0.2)
        state = ui(api)
    assert test(state), f"{what}: {state}"
    return state


def player(api) -> dict:
    return api.get("/api/v2/player").data


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])

    def clean():
        for screen in range(3, 9):
            uart.command("ui screen", {"screen": screen, "up": False})
        for notice in range(7):
            uart.command("ui notice", {"notice": notice, "up": False})
        for name in FILES:
            api.call("POST", f"/api/v2/test/local?name={name}")
        uart.command("sched auto off")
        uart.command("settings set", {"dwell_s": 30})

    clean()
    assert uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
    yield uart, api
    clean()
    uart.command("sched activate", LOCAL_ONLY)
    api.call("POST", "/api/v2/test/play/stop")


def relist(uart, api, records: int) -> None:
    assert uart.command("sched refresh 1") == {"result": "ok"}
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        channel = player(api)["channels"][0]
        if channel["state"] == "ready" and channel["records"] == records:
            return
        time.sleep(0.2)
    raise AssertionError(f"the folder was not listed with {records} records")


def test_the_notices_about_playback_follow_the_player(rig):
    uart, api = rig
    # An empty folder: the whole screen says so, and why.
    relist(uart, api, 0)
    api.call("POST", "/api/v2/test/play/stop")
    empty = wait_ui(api, "nothing to play is said", lambda s: s.get("notice") == "nothing_to_play")
    assert empty["screen"] == "notice" and empty["covers_artwork"] and not empty["banner"]
    assert empty["title"] == "Nothing to play" and "Add artworks" in empty["text"]

    # A file arrives and plays: the notice is gone without anyone taking it down.
    for name, source in FILES.items():
        body = (CORPUS / "good" / source).read_bytes()
        assert api.call("POST", f"/api/v2/test/local?name={name}", body, timeout=30).status == 200
    relist(uart, api, 2)
    playing = wait_ui(api, "the artwork is all there is", lambda s: s["screen"] == "artwork" and "notice" not in s)
    assert not playing["covers_artwork"] and "text" not in playing

    # A named artwork that cannot be shown: a banner over what plays, for a while.
    assert api.call("POST", "/api/v2/player/next").status == 200
    time.sleep(1.5)
    gone = player(api)["current"]["name"]
    assert api.call("POST", "/api/v2/player/next").status == 200
    time.sleep(1.5)
    api.call("POST", f"/api/v2/test/local?name={gone}")
    assert api.call("POST", "/api/v2/player/back").status == 200
    banner = wait_ui(api, "the failure is said", lambda s: s.get("notice") == "play_problem")
    assert banner["screen"] == "artwork" and banner["banner"] and not banner["covers_artwork"]
    assert banner["title"] == "Not shown" and "no longer on the device" in banner["text"]
    wait_ui(api, "and goes by itself", lambda s: "notice" not in s, timeout=12)


def test_the_screen_on_top_is_seen_and_taking_it_down_uncovers_the_next(rig):
    uart, api = rig
    start = ui(api)
    uart.command("ui screen", {"screen": INFO, "up": True, "title": "Info", "text": "p3a-fab"})
    assert ui(api)["screen"] == "info"
    uart.command("ui screen", {"screen": UPDATE, "up": True, "title": "Updating", "text": "Do not unplug", "progress": 40})
    top = ui(api)
    assert (top["screen"], top["text"], top["progress"]) == ("update", "Do not unplug", 40)
    assert top["covers_artwork"] and top["revision"] > start["revision"]
    uart.command("ui screen", {"screen": SETUP, "up": True, "text": "underneath"})
    assert ui(api)["revision"] == top["revision"], "a change underneath is not a change on the screen"
    uart.command("ui screen", {"screen": UPDATE, "up": False})
    assert ui(api)["screen"] == "setup"
    uart.command("ui screen", {"screen": SETUP, "up": False})
    assert ui(api)["screen"] == "info"
    uart.command("ui screen", {"screen": INFO, "up": False})
    assert ui(api)["screen"] == "artwork"


def test_a_screen_over_the_artwork_holds_playback_and_a_banner_does_not(rig):
    uart, api = rig
    uart.command("settings set", {"dwell_s": 2})
    uart.command("sched auto on")
    try:
        assert api.call("POST", "/api/v2/player/next").status == 200
        time.sleep(1.0)
        assert "advance_in_s" in player(api)
        uart.command("ui notice", {"notice": CARD_REPAIRED, "up": True, "text": "The card was repaired", "ttl_ms": 1500})
        assert ui(api)["banner"] and "advance_in_s" in player(api), "a banner leaves the artwork its timer"
        uart.command("ui screen", {"screen": INFO, "up": True, "text": "p3a-fab"})
        held = player(api)
        assert "advance_in_s" not in held
        time.sleep(3.0)
        assert player(api)["counters"]["shown"] == held["counters"]["shown"], "nothing moved on under the screen"
        assert "notice" not in ui(api), "and the banner's time ran out meanwhile"
        uart.command("ui screen", {"screen": INFO, "up": False})
        assert player(api).get("advance_in_s") == 2, "a full dwell once it is uncovered"
    finally:
        uart.command("ui screen", {"screen": INFO, "up": False})
        uart.command("sched auto off")
        uart.command("settings set", {"dwell_s": 30})


def test_the_boot_animations_are_listed_by_name(rig):
    uart, api = rig
    names = api.get("/api/v2/ui/intro-animations").data["animations"]
    assert len(names) == 22 and len(set(names)) == 22
    assert "smoothstep-fade" in names and all(n == n.lower() and " " not in n for n in names)


def test_a_capped_notice_is_shown_only_so_many_times_per_boot(rig):
    uart, api = rig
    spec = {"notice": SOURCE_PROBLEM, "up": True, "text": "Too many requests", "ttl_ms": 600, "max_per_boot": 2}
    shown = 0
    for _ in range(3):
        uart.command("ui notice", spec)
        if ui(api).get("notice") == "source_problem":
            shown += 1
            wait_ui(api, "it goes by itself", lambda s: "notice" not in s, timeout=5)
    # Two on a fresh boot; fewer when this test already ran since the restart.
    assert shown <= 2
    uart.command("ui notice", spec)
    assert "notice" not in ui(api), "past the cap it is not shown"
