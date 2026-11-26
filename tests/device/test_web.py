# SPDX-License-Identifier: Apache-2.0
"""What the web pages are served from and ask for: the pages themselves, the
manifest, and the artworks' bytes for the previews.

The pages' own logic is tested with node (tests/web). This is the device's
side: every file the home page loads is there with the right type, and a
record's "file" address gives exactly the picture the device plays.
"""
import gzip
import hashlib
import re
import time

import pytest

import lab
from test_api import Api
from test_decoders import CORPUS
from test_scheduler import LOCAL_ONLY, Rig

pytestmark = pytest.mark.live


@pytest.fixture(scope="module")
def rig(bring_up):
    uart = bring_up[0]
    r = Rig(uart, Api(lab.provision(uart)["ip"]))
    uart.command("settings set", {"dwell_s": 600})
    yield r
    uart.command("sched activate", LOCAL_ONLY)
    uart.command("settings set", {"dwell_s": 30})


def text(reply) -> str:
    """A page or a script as text: the device sends them gzipped, as a browser asks."""
    body = gzip.decompress(reply.body) if reply.headers.get("content-encoding") == "gzip" else reply.body
    return body.decode()


def test_the_home_page_and_everything_it_loads_are_served(rig):
    api = rig.api
    page = api.get("/")
    assert page.status == 200 and page.headers["content-type"].startswith("text/html")
    assert page.headers.get("content-encoding") == "gzip"
    html = text(page)
    assert "<title>p3a</title>" in html
    assert api.get("/index").body == page.body and api.get("/index.html").body == page.body
    # A second asking with the tag the first gave is a few bytes.
    again = api.get("/", headers={"If-None-Match": page.headers["etag"]})
    assert again.status == 304 and again.body == b""
    wanted = set(re.findall(r'(?:href|src)="(/[^"]+)"', html))
    assert {"/static/common.css", "/static/home.css", "/static/theme.js", "/static/home.js", "/favicon.png"} <= wanted
    types = {".css": "text/css", ".js": "text/javascript", ".png": "image/png", ".webmanifest": "application/manifest+json"}
    for path in sorted(wanted):
        reply = api.get(path)
        assert reply.status == 200 and len(reply.body) > 0, path
        ending = path[path.rindex("."):]
        assert reply.headers["content-type"].startswith(types[ending]), path
    # What the page's script imports in its turn.
    script = text(api.get("/static/home.js"))
    for module in re.findall(r"from '\./([a-z]+\.js)'", script):
        reply = api.get(f"/static/{module}")
        assert reply.status == 200 and reply.headers["content-type"].startswith("text/javascript"), module
    for icon in ("pin", "unpin", "submit_emoji_reaction", "revoke_emoji_reaction", "submit_click", "reaction-error"):
        assert api.get(f"/static/reactions/{icon}.png").status == 200, icon
    for missing in ("/static/", "/static/common", "/static/../index.html", "/nothing", "/static/home.js/"):
        assert api.get(missing).status == 404, missing


def test_the_manifest_names_the_app_after_the_device(rig):
    reply = rig.api.get("/manifest.webmanifest")
    assert reply.status == 200 and reply.headers["content-type"] == "application/manifest+json"
    manifest = reply.json
    assert manifest["name"] == manifest["short_name"] == "p3a-fab"
    assert manifest["start_url"] == "/" and manifest["display"] == "standalone"
    assert len(manifest["icons"]) == 4
    for icon in manifest["icons"]:
        got = rig.api.get(icon["src"])
        assert got.status == 200 and got.body[:8] == b"\x89PNG\r\n\x1a\n", icon["src"]
        assert len(got.body) < 20000, "the icons were shrunk: they are most of what the pages weigh"


def test_a_file_of_the_users_own_is_served_to_the_page(rig):
    api = rig.api
    body = (CORPUS / "good" / "png/rgb.png").read_bytes()
    name = "zz web test.png"
    rig.put(name.replace(" ", "%20"), body)
    try:
        assert rig.uart.command("sched activate", LOCAL_ONLY) == {"result": "ok"}
        rig.uart.command("sched refresh 1")
        rig.wait("it plays", lambda s: s["playing"] and not s["loading"] and s["current"] == name)
        current = api.get("/api/v2/player").data["current"]
        assert current["file"] == "/api/v2/files/zz%20web%20test.png", "the name is escaped for an address"
        assert current["pinned"] is False
        got = api.get(current["file"])
        assert got.status == 200 and got.body == body and got.headers["content-type"] == "image/png"
        assert got.headers["cache-control"] == "no-cache", "the same name may hold another picture tomorrow"
        again = api.get(current["file"], headers={"If-None-Match": got.headers["etag"]})
        assert again.status == 304 and again.body == b""
        # A like for a file of one's own is nothing.
        liked = api.call("POST", "/api/v2/player/like")
        assert liked.status == 200 and liked.data["reaction"] == "none" and liked.data["name"] == name
    finally:
        rig.remove(name.replace(" ", "%20"))
    for bad in ("nothing-here.png", "..%2Fsettings.png", "a%2Fb.png", "notes.txt", "zz%20web%20test.png"):
        assert api.get(f"/api/v2/files/{bad}").status == 404, bad


def test_an_artwork_from_the_store_is_served_by_the_name_of_its_content(rig):
    api = rig.api
    channel = {"name": "", "channels": [{"source": "makapix", "kind": "promoted"}]}
    rig.uart.command("settings set", {"makapix_cache_size": 32})
    try:
        assert rig.uart.command("sched activate", channel) == {"result": "ok"}
        rig.wait("a Makapix artwork plays", lambda s: s["playing"] and not s["loading"]
                 and s["current_source"] == "makapix", timeout=120)
        state = api.get("/api/v2/player").data
        current = state["current"]
        assert re.fullmatch(r"/api/v2/blobs/[0-9a-f]{32}\.(webp|gif|png|bmp)", current["file"]), current
        assert re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", current["storage_key"])
        got = api.get(current["file"], timeout=30)
        assert got.status == 200 and got.headers["content-type"] == f"image/{current['format']}"
        # The address is the content's name: what it serves can never change.
        reference = current["file"].rsplit("/", 1)[1].split(".")[0]
        assert hashlib.sha256(got.body).hexdigest()[:32] == reference
        assert "immutable" in got.headers["cache-control"]
        again = api.get(current["file"], headers={"If-None-Match": got.headers["etag"]})
        assert again.status == 304
        # What the page needs of the channel is in the same answer.
        ch = state["channels"][0]
        assert ch["source"] == "makapix" and ch["kind"] == "promoted" and ch["identifier"] == "" and "display_name" in ch
        assert isinstance(state["shuffle"], bool) and isinstance(state["pins_revision"], int)
        # Pinned from the page, it says so.
        assert api.call("POST", "/api/v2/pins/active/pin").status == 200
        try:
            deadline = time.monotonic() + 5
            while not api.get("/api/v2/player").data["current"]["pinned"] and time.monotonic() < deadline:
                time.sleep(0.2)
            after = api.get("/api/v2/player").data
            assert after["current"]["pinned"] is True and after["pins_revision"] > state["pins_revision"]
            pins = api.get("/api/v2/pins/active").data["pins"]
            assert pins[-1]["file"] == current["file"], "a pin's thumbnail is the same address"
        finally:
            api.call("POST", "/api/v2/pins/active/unpin", {})
    finally:
        rig.uart.command("settings set", {"makapix_cache_size": 2048})
    for bad in ("0" * 32 + ".webp", "zz.webp", "0" * 32, "0" * 32 + ".exe", "..%2F..%2Fsettings.webp"):
        assert api.get(f"/api/v2/blobs/{bad}").status == 404, bad
