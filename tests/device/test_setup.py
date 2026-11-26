# SPDX-License-Identifier: Apache-2.0
"""Setup: the access point the device opens when it has no network, the page
it serves there, and the Wi-Fi endpoints.

The workstation never joins the setup network (it would have to leave its
own). The access point is checked from outside by scanning for it, and from
inside through test commands that ask the device's own DNS and HTTP servers
over loopback, as a phone on that network would. Joining it with a phone is
on the manual checklist.
"""
import gzip
import subprocess
import time
from pathlib import Path

import pytest

import lab
from test_api import Api

pytestmark = pytest.mark.live

PAGE = Path(__file__).resolve().parents[2] / "components" / "p3a_net" / "static" / "setup" / "index.html"

# What phones and computers fetch to find out whether they are behind a portal.
PROBES = ("/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
          "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt")


def reboot(uart) -> list[str]:
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    return uart.lines[start:]


def wait_state(uart, wanted: str, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    status = uart.command("net status")
    while status["state"] != wanted and time.monotonic() < deadline:
        time.sleep(2)
        status = uart.command("net status")
    return status


def wait_access_point(uart, timeout: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout
    ap = uart.command("net setup")
    while not ap["ap"] and time.monotonic() < deadline:
        time.sleep(1)
        ap = uart.command("net setup")
    return ap


@pytest.fixture
def api(uart):
    return Api(lab.provision(uart)["ip"])


@pytest.fixture(scope="module", autouse=True)
def leave_online(bring_up):
    yield
    lab.provision(bring_up[0])


# --- while the device is on the Wi-Fi -------------------------------------------

def test_the_setup_page_is_served_compressed_and_revalidates(api):
    reply = api.get("/setup/", headers={"Accept-Encoding": "gzip"})
    assert reply.status == 200
    assert reply.headers["content-type"] == "text/html; charset=utf-8"
    assert reply.headers["content-encoding"] == "gzip"
    assert gzip.decompress(reply.body) == PAGE.read_bytes(), "the page in the repository, byte for byte"
    assert len(reply.body) < 4096, "small enough to arrive in a few packets"
    assert api.get("/setup/index.html").headers["etag"] == reply.headers["etag"]
    again = api.get("/setup/", headers={"If-None-Match": reply.headers["etag"]})
    assert again.status == 304 and again.body == b""


def test_on_the_lan_unknown_pages_are_not_redirected(api):
    for path in ("/generate_204", "/hotspot-detect.html", "/setup", "/setup/missing.html"):
        api.get(path).error(404, "NOT_FOUND")
    # "/" is the home page here, not the setup page it is in setup mode.
    home = api.get("/")
    assert home.status == 200 and home.headers["content-type"].startswith("text/html")


def test_wifi_status_over_http(api):
    data = api.get("/api/v2/wifi").data
    assert data["state"] == "up" and data["setup"] is False and data["has_credentials"] is True
    assert data["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"
    assert data["ssid"] == lab.load_secrets()["wifi"]["ssid"]
    assert "password" not in data


def test_put_wifi_checks_everything_before_storing_anything(api, uart):
    before = uart.command("net status")
    api.call("PUT", "/api/v2/wifi", {"password": "x"}).error(400, "INVALID_SSID")
    api.call("PUT", "/api/v2/wifi", {"ssid": ""}).error(400, "INVALID_SSID")
    api.call("PUT", "/api/v2/wifi", {"ssid": "s" * 33}).error(400, "INVALID_SSID")
    api.call("PUT", "/api/v2/wifi", {"ssid": 7}).error(400, "INVALID_SSID")
    api.call("PUT", "/api/v2/wifi", {"ssid": "ok", "password": "p" * 65}).error(400, "INVALID_PASSWORD")
    api.call("PUT", "/api/v2/wifi", {"ssid": "ok", "password": 1}).error(400, "INVALID_PASSWORD")
    api.call("PUT", "/api/v2/wifi", {"ssid": "ok", "device_name": "Not Valid"}).error(400, "INVALID_SETTING")
    api.call("PUT", "/api/v2/wifi", {"ssid": "ok", "device_name": 5}).error(400, "INVALID_SETTING")
    api.call("PUT", "/api/v2/wifi", b"ssid=x").error(400, "INVALID_JSON")
    time.sleep(2.5)  # a restart would have happened by now
    after = uart.command("net status")
    assert after["ssid"] == before["ssid"] and after["hostname"] == before["hostname"] and after["up"] is True
    assert reboot(uart) and lab.wait_online(uart)["ssid"] == before["ssid"], "the stored network is untouched"


@pytest.mark.slow
def test_put_wifi_stores_names_and_restarts(api, uart):
    wifi = lab.load_secrets()["wifi"]
    start = uart.mark()
    reply = api.call("PUT", "/api/v2/wifi", {"ssid": wifi["ssid"], "password": wifi["password"], "device_name": "lab"})
    assert reply.data == {"hostname": "p3a-lab", "restarting": True}
    uart.wait_ready(since=start, timeout=30)
    status = lab.wait_online(uart)
    assert status["hostname"] == "p3a-lab"
    assert not any(wifi["password"] in line for line in uart.lines[start:])
    # Back to the dev unit's own name, the same way.
    start = uart.mark()
    reply = Api(status["ip"]).call("PUT", "/api/v2/wifi", {"ssid": wifi["ssid"], "password": wifi["password"],
                                                          "device_name": lab.DEV_UNIT_NAME})
    assert reply.data["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"
    uart.wait_ready(since=start, timeout=30)
    assert lab.wait_online(uart)["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"


# --- setup ----------------------------------------------------------------------

@pytest.mark.slow
def test_delete_wifi_restarts_into_setup_with_a_working_portal(api, uart):
    start = uart.mark()
    assert api.call("DELETE", "/api/v2/wifi").data == {"restarting": True}
    uart.wait_ready(since=start, timeout=30)
    status = uart.command("net status")
    assert status["state"] == "setup" and status["has_credentials"] is False

    ap = wait_access_point(uart)
    assert ap == {"ap": True, "ssid": "p3a-setup", "open": True, "channel": 1, "ip": "192.168.4.1"}
    assert any('join the Wi-Fi network "p3a-setup"' in line for line in uart.lines[start:])

    # Every name resolves to the device.
    for name in ("captive.apple.com", "connectivitycheck.gstatic.com", "www.msftconnecttest.com", "p3a.local", "x"):
        answer = uart.command(f"net selfdns {name}")
        assert answer["answered"] is True and answer["answers"] == 1 and answer["ip"] == "192.168.4.1", name

    # Every probe, and any other unknown page, is sent to the setup page by address.
    for path in PROBES + ("/some/page/from/another/network", "/favicon.ico"):
        got = uart.command(f"net selfhttp {path}")
        assert got["status"] == 302 and got["location"] == "http://192.168.4.1/", path
    # The setup page itself, at the root.
    page = uart.command("net selfhttp /")
    assert page["status"] == 200 and page["encoding"] == "gzip" and page["type"].startswith("text/html")
    assert uart.command("net selfhttp /setup/index.html")["status"] == 200
    # The API keeps its own answers, so that the page's calls get JSON, not a redirect.
    assert uart.command("net selfhttp /api/v2/wifi") == {"status": 200, "location": "", "encoding": "",
                                                         "type": "application/json"}
    assert uart.command("net selfhttp /api/v2/nothing")["status"] == 404
    assert uart.command("mem")["level"] == "ok"


@pytest.mark.slow
def test_the_setup_network_is_on_the_air(uart):
    uart.command("net forget")
    reboot(uart)
    assert wait_access_point(uart)["ap"] is True
    seen = False
    # Windows refreshes its list of networks on its own schedule, about once a
    # minute while it is connected; a new network can take that long to show.
    deadline = time.monotonic() + 150
    while not seen and time.monotonic() < deadline:
        scan = subprocess.run(["netsh", "wlan", "show", "networks"], capture_output=True, text=True)
        if "SSID" not in scan.stdout:
            pytest.skip("this workstation has no Wi-Fi adapter to scan with")
        seen = "p3a-setup" in scan.stdout
        if not seen:
            time.sleep(5)
    assert seen, "the workstation's Wi-Fi scan should list p3a-setup"


@pytest.mark.slow
def test_a_refused_password_leads_to_setup_quickly(uart):
    wifi = lab.load_secrets()["wifi"]
    uart.command("settings set", {"device_name": lab.DEV_UNIT_NAME})
    uart.command("net join", {"ssid": wifi["ssid"], "password": "definitely-not-the-password"})
    boot_at = time.monotonic()
    start = uart.mark()
    reboot(uart)
    status = wait_state(uart, "setup", timeout=75)
    assert status["state"] == "setup", "three refusals, then setup"
    assert time.monotonic() - boot_at < 80, "someone who mistyped a password is not kept waiting"
    assert status["up_events"] == 0
    assert any("it refused the connection" in line for line in uart.lines[start:])
    assert wait_access_point(uart)["ap"] is True, "the same driver became the access point"
    assert uart.command("net selfhttp /generate_204")["status"] == 302


@pytest.mark.slow
def test_an_absent_network_is_given_time_before_setup(uart):
    uart.command("net join", {"ssid": "p3a-lab-no-such-network", "password": "not-a-real-password"})
    start = uart.mark()
    reboot(uart)
    began = time.monotonic()
    # A router restarting after a power cut would still be coming up now.
    time.sleep(60)
    assert uart.command("net status")["state"] == "connecting", "still trying after a minute"
    assert uart.command("net setup")["ap"] is False
    status = wait_state(uart, "setup", timeout=120)
    waited = time.monotonic() - began
    assert status["state"] == "setup"
    assert 110 < waited < 175, f"about two minutes, not {waited:.0f} s"
    assert any("it is not in range" in line for line in uart.lines[start:])
    assert status["reinits"] == 0, "no driver re-initialisation before the first connection"
    assert wait_access_point(uart)["ap"] is True
