# SPDX-License-Identifier: Apache-2.0
"""The HTTP API over the real network: /api/v2 as a client sees it.

The workstation talks to the unit at the address it reports on the UART, and
the UART is used to cross-check what HTTP says and does.
"""
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

import lab

pytestmark = pytest.mark.live

SCHEMA_FILE = Path(__file__).resolve().parents[2] / "components" / "p3a_settings" / "settings.json"
SCHEMA = json.loads(SCHEMA_FILE.read_text(encoding="utf-8"))["settings"]
DEFAULTS = {s["key"]: s["default"] for s in SCHEMA}


class Reply:
    def __init__(self, status: int, headers, body: bytes):
        self.status = status
        self.headers = {k.lower(): v for k, v in headers.items()}
        self.body = body

    @property
    def json(self) -> dict:
        return json.loads(self.body)

    @property
    def data(self) -> dict:
        doc = self.json
        assert doc["ok"] is True and set(doc) == {"ok", "data"}, doc
        return doc["data"]

    def error(self, status: int, code: str) -> str:
        """Asserts the error envelope and returns its message."""
        assert self.status == status, (self.status, self.body[:200])
        doc = self.json
        assert doc["ok"] is False and set(doc) == {"ok", "error", "code"}, doc
        assert doc["code"] == code, doc
        assert self.headers["content-type"] == "application/json"
        return doc["error"]


class Api:
    def __init__(self, ip: str):
        self.base = f"http://{ip}"

    def call(self, method: str, path: str, body=None, headers=None, timeout: float = 10.0) -> Reply:
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return Reply(response.status, response.headers, response.read())
        except urllib.error.HTTPError as error:
            return Reply(error.code, error.headers, error.read())

    def get(self, path: str, **kw) -> Reply:
        return self.call("GET", path, **kw)


def reboot(uart) -> list[str]:
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    return uart.lines[start:]


@pytest.fixture
def api(uart):
    """The unit online, and a client for it."""
    status = lab.provision(uart)
    return Api(status["ip"])


@pytest.fixture(scope="module", autouse=True)
def leave_clean(bring_up):
    yield
    uart = bring_up[0]
    lab.provision(uart)
    uart.command("settings set", {k: v for k, v in DEFAULTS.items() if k not in ("device_name",)})


# --- the envelope and the router ------------------------------------------------

def test_status_describes_the_device(api, uart, identity):
    reply = api.get("/api/v2/status")
    assert reply.status == 200 and reply.headers["content-type"] == "application/json"
    data = reply.data
    seen = uart.command("net status")
    assert data["version"] == identity["version"]
    assert data["network"]["state"] == "up" and data["network"]["ip"] == seen["ip"]
    assert data["network"]["hostname"] == f"p3a-{lab.DEV_UNIT_NAME}"
    assert data["time"]["set"] is True and data["time"]["now"].endswith("Z")
    assert data["storage"]["health"] == "ok" and data["storage"]["root"] == "/sdcard/p3a"
    assert data["storage"]["total_mb"] > 1000 and 0 < data["storage"]["free_mb"] <= data["storage"]["total_mb"]
    assert data["memory"]["level"] == "ok" and data["memory"]["free"] > 100_000
    assert data["recovery_restarts"] == {"touch": 0, "wifi": 0, "link": 0}
    assert data["uptime_s"] >= 0 and data["reboot_needed"] is False
    assert data["coredump"]["checked"] is True


def test_unknown_paths_and_wrong_methods_get_the_error_envelope(api):
    # "/" is the home page (test_web.py).
    for path in ("/nothing", "/api/v2", "/api/v2/", "/api/v2/status/", "/api/v1/status", "/API/v2/status"):
        api.get(path).error(404, "NOT_FOUND")
    reply = api.call("POST", "/api/v2/status")
    reply.error(405, "METHOD_NOT_ALLOWED")
    assert reply.headers["allow"] == "GET"
    reply = api.call("DELETE", "/api/v2/settings")
    reply.error(405, "METHOD_NOT_ALLOWED")
    assert reply.headers["allow"] == "GET, PATCH"
    api.get("/" + "a" * 300).error(414, "URI_TOO_LONG")


def test_a_query_string_does_not_change_the_route(api):
    assert api.get("/api/v2/status?verbose=1&x=%20").status == 200
    assert api.get("/api/v2/jobs/1?x=1").status == 200


def test_get_answers_carry_an_etag_and_honour_if_none_match(api, uart):
    first = api.get("/api/v2/settings")
    etag = first.headers["etag"]
    assert etag.startswith('W/"') and first.headers["cache-control"] == "no-cache"
    again = api.get("/api/v2/settings", headers={"If-None-Match": etag})
    assert again.status == 304 and again.body == b""
    assert again.headers["etag"] == etag
    assert api.get("/api/v2/settings", headers={"If-None-Match": 'W/"00000000"'}).status == 200
    # A change gives a new ETag, and the old one no longer matches.
    assert api.call("PATCH", "/api/v2/settings", {"brightness": 55}).status == 200
    changed = api.get("/api/v2/settings", headers={"If-None-Match": etag})
    assert changed.status == 200 and changed.headers["etag"] != etag
    assert changed.data["brightness"] == 55
    api.call("PATCH", "/api/v2/settings", {"brightness": 100})


# --- settings -------------------------------------------------------------------

def test_settings_over_http_match_the_store(api, uart):
    assert api.get("/api/v2/settings").data == uart.command("settings get")
    assert set(api.get("/api/v2/settings").data) == set(DEFAULTS)


def test_patch_changes_saves_and_reports(api, uart):
    reply = api.call("PATCH", "/api/v2/settings", {"dwell_s": 45, "show_fps": True, "giphy_rating": "g"})
    assert reply.data == {"changed": ["show_fps", "dwell_s", "giphy_rating"], "reboot_needed": False}
    stored = uart.command("settings get")
    assert (stored["dwell_s"], stored["show_fps"], stored["giphy_rating"]) == (45, True, "g")
    # Writing the same values again changes nothing.
    again = api.call("PATCH", "/api/v2/settings", {"dwell_s": 45, "show_fps": True})
    assert again.data["changed"] == []
    api.call("PATCH", "/api/v2/settings", {"dwell_s": 30, "show_fps": False, "giphy_rating": "pg-13"})


def test_patch_is_all_or_nothing_and_says_why(api, uart):
    before = uart.command("settings get")
    message = api.call("PATCH", "/api/v2/settings", {"dwell_s": 45, "rotation": 45}).error(400, "INVALID_SETTING")
    assert "rotation" in message
    message = api.call("PATCH", "/api/v2/settings", {"dwell_s": 45, "dwell_time_ms": 1}).error(400, "UNKNOWN_SETTING")
    assert message == "dwell_time_ms is not a setting"
    api.call("PATCH", "/api/v2/settings", {"brightness": "bright"}).error(400, "INVALID_SETTING")
    assert uart.command("settings get") == before


def test_bodies_that_are_not_a_json_object_are_refused(api, uart):
    before = uart.command("settings get")
    for body in (b"", b"brightness=5", b"[1,2]", b'"x"', b'{"brightness":', b"null"):
        api.call("PATCH", "/api/v2/settings", body).error(400, "INVALID_JSON")
    too_big = b'{"device_name":"' + b"x" * 33000 + b'"}'
    api.call("PATCH", "/api/v2/settings", too_big).error(413, "TOO_LARGE")
    assert uart.command("settings get") == before
    assert api.get("/api/v2/status").status == 200, "the server is fine afterwards"


def test_secrets_are_masked_and_only_the_museum_keys_have_their_own_call(api, uart):
    keys = {"giphy_api_key": "g-test-key-000111", "klipy_api_key": "k-test-key-000222",
            "harvard_api_key": "h-test-key-000333", "smithsonian_api_key": "s-test-key-000444"}
    try:
        assert sorted(api.call("PATCH", "/api/v2/settings", keys).data["changed"]) == sorted(keys)
        shown = api.get("/api/v2/settings")
        for key in keys:
            assert shown.data[key] == "********"
        assert not any(value.encode() in shown.body for value in keys.values())

        reply = api.get("/api/v2/secrets/museum-keys")
        assert reply.data == {"harvard_api_key": keys["harvard_api_key"],
                              "smithsonian_api_key": keys["smithsonian_api_key"]}
        assert reply.headers["cache-control"] == "no-store" and "etag" not in reply.headers
        # Nothing else hands a key out.
        status = api.get("/api/v2/status")
        assert not any(value.encode() in status.body for value in keys.values())
    finally:
        api.call("PATCH", "/api/v2/settings", {key: "" for key in keys})
    assert api.get("/api/v2/secrets/museum-keys").data == {"harvard_api_key": "", "smithsonian_api_key": ""}


def test_the_schema_is_served_as_written(api):
    reply = api.get("/api/v2/settings/schema")
    assert reply.status == 200 and reply.headers["content-type"] == "application/json"
    assert reply.body == SCHEMA_FILE.read_bytes(), "byte for byte the file in the repository"
    again = api.get("/api/v2/settings/schema", headers={"If-None-Match": reply.headers["etag"]})
    assert again.status == 304 and again.body == b""


# --- jobs -----------------------------------------------------------------------

def test_jobs_are_readable_by_id(api):
    job = api.get("/api/v2/jobs/1").data
    assert job["id"] == 1 and job["state"] == "done" and job["succeeded"] is True and job["progress"] == 100
    assert job["name"], "every job has a name; which one is first depends on the boot order"
    for missing in ("999999", "0", "abc", "1x", "-1", "4294967297"):
        api.get(f"/api/v2/jobs/{missing}").error(404, "NOT_FOUND")


# --- system ---------------------------------------------------------------------

def test_reboot_over_http(api, uart):
    start = uart.mark()
    assert api.call("POST", "/api/v2/system/reboot").data == {}
    uart.wait_ready(since=start, timeout=30)
    status = lab.wait_online(uart)
    assert Api(status["ip"]).get("/api/v2/status").data["uptime_s"] < 30


def test_factory_reset_needs_the_confirmation(api, uart):
    api.call("PATCH", "/api/v2/settings", {"dwell_s": 77})
    api.call("POST", "/api/v2/system/factory-reset", {}).error(400, "NOT_CONFIRMED")
    api.call("POST", "/api/v2/system/factory-reset", {"confirm": "yes"}).error(400, "NOT_CONFIRMED")
    api.call("POST", "/api/v2/system/factory-reset", b"").error(400, "INVALID_JSON")
    time.sleep(1.5)
    assert uart.command("settings get")["dwell_s"] == 77, "nothing was erased"
    assert uart.command("net status")["has_credentials"] is True
    api.call("PATCH", "/api/v2/settings", {"dwell_s": 30})


@pytest.mark.slow
def test_factory_reset_over_http(api, uart):
    api.call("PATCH", "/api/v2/settings", {"dwell_s": 77})
    start = uart.mark()
    reply = api.call("POST", "/api/v2/system/factory-reset", {"confirm": "factory-reset"})
    assert reply.status == 202 and reply.data["job"] > 0
    uart.wait_ready(since=start, timeout=40)
    assert uart.command("settings get") == DEFAULTS
    net = uart.command("net status")
    assert net["state"] == "setup" and net["has_credentials"] is False
    lab.provision(uart)


# --- crash dumps ----------------------------------------------------------------

@pytest.mark.slow
@pytest.mark.expects_crash
def test_a_crash_dump_can_be_seen_downloaded_and_erased(api, uart):
    api.get("/api/v2/system/coredump").error(404, "NOT_FOUND")
    assert api.get("/api/v2/status").data["coredump"] == {"checked": True, "present": False}

    start = uart.mark()
    uart.command("crash")
    uart.wait_ready(since=start, timeout=60)
    assert any("abort() was called" in line for line in uart.lines[start:])
    status = lab.wait_online(uart)
    api = Api(status["ip"])

    deadline = time.monotonic() + 20
    dump = api.get("/api/v2/status").data["coredump"]
    while not dump["checked"] and time.monotonic() < deadline:
        time.sleep(1)
        dump = api.get("/api/v2/status").data["coredump"]
    assert dump["present"] is True
    assert dump["task"] == "p3a_test_con", "the task that called abort()"
    assert dump["pc"].startswith("0x") and dump["bytes"] > 1000

    reply = api.get("/api/v2/system/coredump", timeout=30)
    assert reply.status == 200 and reply.headers["content-type"] == "application/octet-stream"
    assert reply.headers["cache-control"] == "no-store"
    assert len(reply.body) == dump["bytes"]
    assert b"\x7fELF" in reply.body[:64], "an ELF core file, after the dump's own header"

    assert api.call("DELETE", "/api/v2/system/coredump").data == {}
    assert api.get("/api/v2/status").data["coredump"] == {"checked": True, "present": False}
    api.get("/api/v2/system/coredump").error(404, "NOT_FOUND")
    # And it stays gone across a restart.
    reboot(uart)
    status = lab.wait_online(uart)
    time.sleep(2)
    assert Api(status["ip"]).get("/api/v2/status").data["coredump"]["present"] is False


# --- behaviour under use --------------------------------------------------------

@pytest.mark.slow
def test_many_requests_leave_no_sockets_or_memory_behind(api, uart):
    before = uart.command("mem")
    for i in range(120):
        assert api.get("/api/v2/status").status == 200
        if i % 3 == 0:
            api.get("/nothing")
    time.sleep(2)
    after = uart.command("mem")
    assert after["level"] == "ok"
    assert before["free"] - after["free"] < 8192, "no steady loss of internal memory"
    assert uart.command("net status")["drops"] == 0
