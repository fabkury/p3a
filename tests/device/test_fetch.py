# SPDX-License-Identifier: Apache-2.0
"""The HTTP fetcher on the device, against real servers: a page into memory,
a file onto the card hashed as it is written and taken into the blob
store, and the failures a caller must tell apart.

The servers are ones that have served these addresses unchanged for years.
The workstation fetches the same file to know what the device should have.
"""
import hashlib
import urllib.request

import pytest

import lab

pytestmark = pytest.mark.live

PICTURE = "https://www.gstatic.com/webp/gallery/1.webp"
WEBP = 1  # p3a_format_t


@pytest.fixture(scope="module")
def online(bring_up):
    uart = bring_up[0]
    lab.provision(uart)
    return uart


def test_a_page_is_fetched_into_memory(online):
    got = online.command("fetch get", {"url": "https://example.com/"}, timeout=60)
    assert got["result"] == "ok" and got["status"] == 200 and got["attempts"] == 1
    assert got["bytes"] > 200 and "Example Domain" in got["start"]


def test_a_file_is_downloaded_hashed_and_taken_into_the_blob_store(online):
    with urllib.request.urlopen(PICTURE, timeout=30) as reply:
        wanted = reply.read()
    sha = hashlib.sha256(wanted).hexdigest()
    got = online.command("fetch blob", {"url": PICTURE, "format": WEBP}, timeout=90)
    try:
        assert got["result"] == "ok" and got["status"] == 200
        assert got["bytes"] == len(wanted) and got["sha256"] == sha, "the hash made while writing is the file's"
        assert got["ref"] == sha[:32], "and the blob is named after it"
        # The same bytes again are not stored twice.
        again = online.command("fetch blob", {"url": PICTURE, "format": WEBP}, timeout=90)
        assert again["result"] == "ok" and again["ref"] == got["ref"] and again["already"] is True
    finally:
        if "ref" in got:
            online.command(f"library blob-remove {got['ref']} {WEBP}")


def test_the_failures_a_caller_must_tell_apart(online):
    missing = online.command("fetch get", {"url": "https://www.gstatic.com/webp/gallery/no-such-p3a-file.webp"}, timeout=60)
    assert missing["result"] == "MISSING" and missing["status"] == 404 and missing["attempts"] == 1, \
        "a 404 is final: it is not tried again"
    nowhere = online.command("fetch get", {"url": "https://no-such-host.invalid/"}, timeout=90)
    assert nowhere["result"] not in ("ok", "MISSING") and nowhere["attempts"] == 3, "a server that cannot be reached is tried three times"
    # A file that fails to download leaves nothing behind in the store.
    blob = online.command("fetch blob", {"url": "https://www.gstatic.com/webp/gallery/no-such-p3a-file.webp",
                                         "format": WEBP}, timeout=60)
    assert blob["result"] == "MISSING" and "ref" not in blob
