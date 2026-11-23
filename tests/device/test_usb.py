# SPDX-License-Identifier: Apache-2.0
"""Sharing the SD card over USB, with the test playing the computer's part.

The session's rules are host-tested (usb_session). No test can plug a
cable in, so the bus's reports are injected through a hook: the device then
does everything it does for a real computer except serve it sectors. What a
real computer sees is manual check M3.6-a.
"""
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = [pytest.mark.live, pytest.mark.slow]

PNG = (CORPUS / "good" / "png" / "rgb.png").read_bytes()


def test_a_session_takes_the_card_and_ends_in_a_restart(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    assert uart.command("ui usb") == {"sharing": False}
    assert api.call("POST", "/api/v2/test/local?name=zz-usb.png", PNG, timeout=30).status == 200
    uart.command("sched activate", {"name": "", "channels": [{"source": "local"}]})
    uart.command("sched refresh 1")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not api.get("/api/v2/player").data["playing"]:
        time.sleep(0.2)

    start = uart.mark()
    assert uart.command("ui usb mounted") == {"sharing": True}
    time.sleep(1.0)
    ui = api.get("/api/v2/ui").data
    assert ui["screen"] == "usb" and "in use by the computer" in ui["text"]
    player = api.get("/api/v2/player").data
    assert player["playing"] is False, "playback stopped: nothing reads the card"
    # Nothing writes to it either, and each refusal says why.
    api.call("POST", "/api/v2/files?name=zz-usb-2.png", PNG).error(409, "CARD_IN_USE")
    api.call("POST", "/api/v2/player/next").error(409, "CARD_IN_USE")
    reply = api.call("PUT", "/api/v2/playsets/zz-usb", {"channels": [{"source": "local"}]})
    assert reply.status >= 400, "a playset is not saved onto a card the computer owns"
    # Gestures do nothing under this screen.
    uart.command("ui touch", [[1, 600, 300, 0, 0, 80], [0, 0, 0, 0, 0, 60]])
    time.sleep(0.5)
    assert uart.command("ui touched")["last_action"] == "none"

    # The computer lets go: two seconds to come back, the notice, the restart.
    uart.command("ui usb unmounted")
    time.sleep(1.0)
    assert "in use by the computer" in api.get("/api/v2/ui").data["text"], "not yet: it may be a bounce"
    uart.wait_for(r"the USB session is over", timeout=5, since=start)
    assert "Restarting" in api.get("/api/v2/ui").data["text"]
    uart.wait_ready(since=start, timeout=30)

    api = Api(lab.provision(uart)["ip"])
    assert uart.command("ui usb") == {"sharing": False}
    assert uart.command("storage info")["health"] == "ok"
    assert api.call("POST", "/api/v2/test/local?name=zz-usb.png").status == 200  # and writes work again
