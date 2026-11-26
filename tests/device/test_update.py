# SPDX-License-Identifier: Apache-2.0
"""The firmware update, end to end, between two firmwares served from a
release mirror on the workstation.

The "new" firmware is the test build itself with another version number
written into it (and its checksum and hash put right), so the device ends
the cycle running the same code. The mirror answers as GitHub does: a list
of releases, and the image. The device is pointed at it by a test command;
no setting and no API can do that.

Which release is chosen, the order of versions, when the device looks and
the trial's verdict are host-tested (test_update_release, test_update_policy).
Not tested here: a firmware that fails its trial (it needs ten minutes
without a network), and GitHub itself beyond one look at its real list.
"""
import hashlib
import http.server
import json
import socket
import threading
import time

import pytest

import lab
from test_api import Api
from test_pins import job_done

pytestmark = pytest.mark.live

NEW = "2.0.1"
DESC = 32          # the app description's place in an image: after the two headers
VERSION_AT = DESC + 16


def with_version(image: bytes, version: str) -> bytes:
    """The image saying it is `version`, with its checksum and its hash made right again."""
    assert image[0] == 0xE9 and image[23] == 1, "an app image with its SHA-256 appended"
    assert hashlib.sha256(image[:-32]).digest() == image[-32:], "the build's own image is whole"
    assert image[DESC:DESC + 4] == bytes.fromhex("3254cdab"), "the app description is where it is expected"
    old = image[VERSION_AT:VERSION_AT + 32]
    new = version.encode().ljust(32, b"\0")
    assert len(new) == 32
    out = bytearray(image[:-32])
    out[VERSION_AT:VERSION_AT + 32] = new
    # The checksum is the XOR of every segment byte: take the old bytes out and put the new in.
    checksum = out[-1]
    for a, b in zip(old, new):
        checksum ^= a ^ b
    out[-1] = checksum
    return bytes(out) + hashlib.sha256(out).digest()


class Mirror:
    """A stand-in for GitHub: /releases lists what `self.releases` holds, /download/... serves `self.files`."""

    def __init__(self, device_ip: str):
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((device_ip, 80))
        self.ip = probe.getsockname()[0]  # this machine, as the device reaches it
        probe.close()
        self.releases: list = []
        self.files: dict = {}
        self.asked: list = []
        mirror = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                mirror.asked.append(self.path)
                if self.path.startswith("/releases"):
                    body = json.dumps(mirror.releases).encode()
                    kind = "application/json"
                elif self.path in mirror.files:
                    body, kind = mirror.files[self.path], "application/octet-stream"
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        self.port = self.server.server_address[1]
        self.base = f"http://{self.ip}:{self.port}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def offer(self, tag: str, image: bytes, digest: str | None = None, prerelease: bool = False, name="p3a-v2.bin"):
        path = f"/download/{tag}/{name}"
        self.files[path] = image
        self.releases.insert(0, {
            "tag_name": tag, "draft": False, "prerelease": prerelease, "published_at": "2027-01-02T03:04:05Z",
            "body": f"Notes for {tag}",
            "assets": [{"name": name, "state": "uploaded", "size": len(image),
                        "digest": "sha256:" + (digest or hashlib.sha256(image).hexdigest()),
                        "browser_download_url": self.base + path}],
        })

    def close(self):
        self.server.shutdown()


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    api = Api(lab.provision(uart)["ip"])
    before = api.get("/api/v2/update").data
    mirror = Mirror(api.base.removeprefix("http://"))
    yield uart, api, mirror, before
    mirror.close()
    # Whatever happened, leave the unit on the firmware it came with.
    api = Api(lab.provision(uart)["ip"])
    now = api.get("/api/v2/update").data
    if now["running"] != before["running"] and now.get("other") == before["running"]:
        start = uart.mark()
        api.call("POST", "/api/v2/update/rollback")
        uart.wait_ready(since=start, timeout=60)
        kept(Api(lab.provision(uart)["ip"]))
    uart.command("settings set", {"update_prereleases": False, "update_auto_check": True})


def point_at(uart, mirror):
    uart.command("update source", {"list": mirror.base + "/releases?per_page=8", "prefix": mirror.base + "/download/"})


def look(api) -> dict:
    job = api.call("POST", "/api/v2/update/check")
    assert job.status == 202, job.body
    assert job_done(api, job.data["job"], timeout=40)["succeeded"] is True
    return api.get("/api/v2/update").data


def kept(api, timeout: float = 60.0) -> dict:
    """Waits until the firmware that runs is no longer on trial."""
    deadline = time.monotonic() + timeout
    status = api.get("/api/v2/update").data
    while status["on_trial"] and time.monotonic() < deadline:
        time.sleep(2)
        status = api.get("/api/v2/update").data
    assert status["on_trial"] is False, "reachable again: the firmware is kept"
    return status


def image() -> bytes:
    return (lab.REPO / lab.CONFIGS["test"].build_dir / "p3a.bin").read_bytes()


def test_the_device_says_what_it_runs_and_looks_at_github(rig, identity):
    uart, api, mirror, before = rig
    assert before["running"] == identity["version"] and before["state"] == "idle" and before["on_trial"] is False
    for nothing in ("install",):
        api.call("POST", f"/api/v2/update/{nothing}").error(409, "NOTHING_TO_INSTALL")
    # GitHub's real list, as it is today: fine to read, and while only 1.x has shipped, nothing for this device.
    uart.command("update source", {})
    status = look(api)
    assert status["checked"] is True and "check_error" not in status and status["checked_ago_s"] < 30
    if "latest" in status:
        assert status["latest"]["version"].startswith("2."), "only a 2.x image is ever offered"


def test_a_list_that_cannot_be_read_is_an_error_and_keeps_what_was_known(rig):
    uart, api, mirror, before = rig
    uart.command("update source", {"list": mirror.base + "/nothing-here", "prefix": mirror.base + "/download/"})
    job = api.call("POST", "/api/v2/update/check").data["job"]
    assert job_done(api, job, timeout=40)["error"] == "MISSING"
    status = api.get("/api/v2/update").data
    assert status["check_error"] == "MISSING" and status["available"] is False and status["state"] == "idle"


def test_what_is_offered_follows_the_versions_and_the_prerelease_setting(rig):
    uart, api, mirror, before = rig
    point_at(uart, mirror)
    mirror.releases.clear()
    assert "latest" not in look(api), "an empty list: nothing to offer, and no error"
    small = b"\xe9" + bytes(300 * 1024)
    mirror.offer("v1.9.9", small)
    mirror.offer("v2.5.0-rc.1", small, prerelease=True)
    mirror.offer("v9.0.0", small, name="p3a-v3.bin")  # a later series: not for this firmware
    status = look(api)
    assert status["latest"]["version"] == "1.9.9" and status["available"] is False, "older than what runs"
    api.call("POST", "/api/v2/update/install").error(409, "NOTHING_TO_INSTALL")
    assert api.call("PATCH", "/api/v2/settings", {"update_prereleases": True}).status == 200
    status = look(api)
    assert status["latest"] == {"version": "2.5.0-rc.1", "prerelease": True, "bytes": len(small),
                                "published": "2027-01-02T03:04:05Z", "notes": "Notes for v2.5.0-rc.1"}
    assert status["available"] is True
    assert api.call("PATCH", "/api/v2/settings", {"update_prereleases": False}).status == 200
    assert look(api)["latest"]["version"] == "1.9.9"


@pytest.mark.parametrize("what, why", [
    ("not an image", "not a firmware image"),
    ("another version inside", "says it is version"),
    ("wrong checksum", "does not match the release's checksum"),
    ("cut short", "the release says"),
])
def test_a_bad_download_is_refused_and_the_device_carries_on(rig, what, why):
    uart, api, mirror, before = rig
    point_at(uart, mirror)
    mirror.releases.clear()
    good = with_version(image(), NEW)
    if what == "not an image":
        mirror.offer("v" + NEW, b"<html>" + bytes(len(good) - 6))
    elif what == "another version inside":
        mirror.offer("v" + NEW, with_version(image(), "2.0.2"))
    elif what == "wrong checksum":
        mirror.offer("v" + NEW, good, digest="0" * 64)
    else:
        mirror.offer("v" + NEW, good)
        mirror.files[f"/download/v{NEW}/p3a-v2.bin"] = good[:len(good) // 2]  # the list promises more than comes
        mirror.releases[0]["assets"][0]["digest"] = "sha256:" + hashlib.sha256(good).hexdigest()
    assert look(api)["available"] is True
    uptime = api.get("/api/v2/status").data["uptime_s"]
    job = api.call("POST", "/api/v2/update/install")
    assert job.status == 202, job.body
    done = job_done(api, job.data["job"], timeout=180)
    assert done["succeeded"] is False, done
    status = api.get("/api/v2/update").data
    assert status["state"] == "idle" and status["running"] == before["running"]
    assert why in status["install_error"]["message"], status["install_error"]
    # The update screen came down (known issue H8), and the device is as it was.
    assert api.get("/api/v2/ui").data["screen"] != "update"
    assert api.get("/api/v2/status").data["uptime_s"] >= uptime, "it did not restart"


def test_a_release_is_installed_tried_kept_and_can_be_gone_back_from(rig):
    uart, api, mirror, before = rig
    point_at(uart, mirror)
    mirror.releases.clear()
    mirror.asked.clear()
    new = with_version(image(), NEW)
    mirror.offer("v" + NEW, new)
    status = look(api)
    assert status["available"] is True and status["latest"]["bytes"] == len(new)

    start = uart.mark()
    job = api.call("POST", "/api/v2/update/install")
    assert job.status == 202, job.body
    # A second asking while it runs is refused.
    api.call("POST", "/api/v2/update/install").error(409, "BUSY")
    api.call("POST", "/api/v2/update/check").error(409, "BUSY")
    seen = set()
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        try:
            now = api.get("/api/v2/update", timeout=3).data
        except OSError:
            break  # it is restarting
        seen.add(now["state"])
        if now["state"] == "installing":
            assert 0 <= now["progress"] <= 100
            assert api.get("/api/v2/ui", timeout=3).data["screen"] == "update"
        elif now["state"] == "idle":
            pytest.fail(f"the install ended without a restart: {now}")
        time.sleep(0.5)
    assert "installing" in seen
    assert f"/download/v{NEW}/p3a-v2.bin" in mirror.asked
    ready = uart.wait_ready(since=start, timeout=90)
    assert ready["version"] == NEW, "the device restarted into the new firmware"
    assert not uart.panics(since=start)

    # On trial until the network is back, then kept.
    api = Api(lab.wait_online(uart, timeout=60)["ip"])
    assert api.get("/api/v2/update").data["on_trial"] is True
    status = kept(api)
    assert status["running"] == NEW and status["other"] == before["running"] and "undone" not in status
    uart.wait_for(r"is reachable: kept", timeout=5, since=start)

    # Kept means kept: an ordinary restart stays on it.
    start = uart.mark()
    uart.command("reboot")
    assert uart.wait_ready(since=start, timeout=40)["version"] == NEW
    api = Api(lab.wait_online(uart, timeout=60)["ip"])
    status = api.get("/api/v2/update").data
    assert status["on_trial"] is False and status["other"] == before["running"]

    # And back.
    start = uart.mark()
    job = api.call("POST", "/api/v2/update/rollback")
    assert job.status == 202, job.body
    ready = uart.wait_ready(since=start, timeout=90)
    assert ready["version"] == before["running"]
    # The firmware gone back to is on trial in its turn (a slot newly chosen always is), and kept the same way.
    api = Api(lab.wait_online(uart, timeout=60)["ip"])
    status = kept(api)
    assert status["running"] == before["running"] and status["other"] == NEW
