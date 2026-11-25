# SPDX-License-Identifier: Apache-2.0
"""The player over HTTP: uploading a file, next and back, saved playsets and
the active playset.

The scheduler and the playset store have their own tests; these are about
the routes saying what those say.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = pytest.mark.live

GIF = (CORPUS / "good" / "gif" / "opaque_anim.gif").read_bytes()
PNG = (CORPUS / "good" / "png" / "rgb.png").read_bytes()
OURS = ["zz-api-a.gif", "zz-api-b.png", "zz api c.PNG"]
LOCAL_ONLY = {"name": "", "channels": [{"source": "local"}]}
MIX = {"channels": [{"source": "makapix", "kind": "no-such-kind", "weight": 3},
                    {"source": "local", "display_name": "My files", "weight": 1}]}


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


def remove_ours(api) -> None:
    for name in OURS:
        api.call("POST", "/api/v2/test/local?name=" + name.replace(" ", "%20"))


@pytest.fixture(scope="module")
def api(bring_up):
    uart = bring_up[0]
    client = Api(lab.provision(uart)["ip"])
    remove_ours(client)
    for saved in client.get("/api/v2/playsets").data["playsets"]:
        client.call("DELETE", "/api/v2/playsets/" + saved["name"].replace(" ", "%20"))
    assert client.call("PUT", "/api/v2/player/playset", LOCAL_ONLY).status == 200
    yield client
    remove_ours(client)
    client.call("PUT", "/api/v2/player/playset", LOCAL_ONLY)
    client.call("POST", "/api/v2/test/play/stop")


def test_an_uploaded_file_is_listed_and_played(api):
    before = wait_player(api, "the folder is listed", lambda s: s["channels"][0]["state"] == "ready")
    count = before["channels"][0]["records"]
    reply = api.call("POST", "/api/v2/files?name=zz-api-a.gif", GIF, timeout=30)
    assert reply.status == 200 and reply.data == {"name": "zz-api-a.gif", "bytes": len(GIF), "playing": True}
    reply = api.call("POST", "/api/v2/files?name=zz%20api%20c.PNG", PNG, timeout=30)
    assert reply.data["name"] == "zz api c.PNG", "a space and capitals are kept as sent"
    state = wait_player(api, "both are listed and one plays",
                        lambda s: s["channels"][0]["records"] == count + 2 and s["playing"] and not s["loading"])
    assert state["channels"][0]["available"] == count + 2
    assert state["current"] == {"source": "local", "name": "zz api c.PNG", "format": "png", "width": 48, "height": 32}, \
        "what was just sent is what plays"

    # The same name again replaces the file; it does not add one.
    assert api.call("POST", "/api/v2/files?name=zz-api-a.gif", PNG + b"\0" * 10, timeout=30).status == 200
    time.sleep(1.0)
    assert wait_player(api, "listed again", lambda s: s["channels"][0]["state"] == "ready")["channels"][0]["records"] == count + 2


def test_uploads_that_are_not_pictures_for_the_card_are_refused(api):
    api.call("POST", "/api/v2/files", GIF).error(400, "INVALID_NAME")
    api.call("POST", "/api/v2/files?name=" + "n" * 120 + ".gif", GIF).error(400, "INVALID_NAME")
    api.call("POST", "/api/v2/files?name=a%22b.gif", GIF).error(400, "INVALID_NAME")      # a quote
    api.call("POST", "/api/v2/files?name=..%2Fx.gif", GIF).error(400, "INVALID_NAME")     # a slash
    api.call("POST", "/api/v2/files?name=notes.txt", GIF).error(415, "UNSUPPORTED_TYPE")
    api.call("POST", "/api/v2/files?name=.hidden.gif", GIF).error(415, "UNSUPPORTED_TYPE")
    api.call("POST", "/api/v2/files?name=zz-api-b.png", b"").error(400, "INVALID")
    assert "zz-api-b.png" not in str(player(api))


def test_next_and_back_over_http(api):
    api.call("POST", "/api/v2/files?name=zz-api-b.png", PNG, timeout=30)
    wait_player(api, "three of ours are listed", lambda s: s["channels"][0]["available"] >= 3 and s["playing"])
    start = player(api)
    assert api.call("POST", "/api/v2/player/next").status == 200
    after = wait_player(api, "next was shown", lambda s: s["counters"]["shown"] == start["counters"]["shown"] + 1
                        and not s["loading"])
    assert after["history"]["count"] >= 2 and after["history"]["position"] == 0
    assert api.call("POST", "/api/v2/player/back").status == 200
    back = wait_player(api, "back was shown", lambda s: s["history"]["position"] == 1 and not s["loading"])
    assert back["current"] == start["current"] or start["history"]["count"] == 0
    # Back as far as the history goes, then once more.
    for _ in range(40):
        if api.call("POST", "/api/v2/player/back").status != 200:
            break
        time.sleep(0.3)
    api.call("POST", "/api/v2/player/back").error(409, "NOTHING_TO_PLAY")


def test_playsets_are_saved_read_and_deleted_over_http(api):
    assert api.get("/api/v2/playsets").data == {"playsets": [], "unreadable": 0, "max": 32}
    api.get("/api/v2/playsets/Evening%20mix").error(404, "MISSING")
    assert api.call("PUT", "/api/v2/playsets/Evening%20mix", MIX).status == 200
    assert api.get("/api/v2/playsets").data["playsets"] == [{"name": "Evening mix", "channels": 2}]
    got = api.get("/api/v2/playsets/Evening%20mix").data
    assert got["name"] == "Evening mix" and [c["source"] for c in got["channels"]] == ["makapix", "local"]
    assert got["channels"][1]["display_name"] == "My files" and len(got["channels"][0]["id"]) == 16

    # What was read can be sent back as it is.
    assert api.call("PUT", "/api/v2/playsets/Evening%20mix", got).status == 200
    reply = api.call("PUT", "/api/v2/playsets/Evening%20mix", {**MIX, "name": "Another"})
    reply.error(400, "INVALID_PLAYSET")
    reply = api.call("PUT", "/api/v2/playsets/x", {"channels": [{"source": "local", "wieght": 2}]})
    assert "wieght" in reply.error(400, "INVALID_PLAYSET"), "the answer names what is wrong"
    api.call("PUT", "/api/v2/playsets/" + "n" * 33, MIX).error(400, "INVALID_PLAYSET")

    assert api.call("DELETE", "/api/v2/playsets/Evening%20mix").status == 200
    api.call("DELETE", "/api/v2/playsets/Evening%20mix").error(404, "MISSING")
    assert api.get("/api/v2/playsets").data["playsets"] == []


def test_the_active_playset_is_set_whole_or_by_a_saved_name(api):
    assert api.call("PUT", "/api/v2/player/playset", {"name": "Direct", **MIX}).status == 200
    active = api.get("/api/v2/player/playset").data
    assert active["name"] == "Direct" and len(active["channels"]) == 2
    state = wait_player(api, "its local channel is listed, the other refused",
                        lambda s: s["channels"][1]["state"] == "ready" and s["channels"][0]["state"] == "failed")
    assert state["playset"] == "Direct"

    assert api.call("PUT", "/api/v2/playsets/Saved%20one", LOCAL_ONLY | {"name": "Saved one"}).status == 200
    assert api.call("PUT", "/api/v2/player/playset", {"saved": "Saved one"}).status == 200
    assert api.get("/api/v2/player/playset").data["name"] == "Saved one"
    api.call("PUT", "/api/v2/player/playset", {"saved": "No such"}).error(404, "MISSING")
    api.call("PUT", "/api/v2/player/playset", {"saved": "Saved one", "channels": []}).error(400, "INVALID_PLAYSET")
    api.call("PUT", "/api/v2/player/playset", {"channels": []}).error(400, "INVALID_PLAYSET")
    assert api.get("/api/v2/player/playset").data["name"] == "Saved one", "a refused request changes nothing"
    assert api.call("DELETE", "/api/v2/playsets/Saved%20one").status == 200
