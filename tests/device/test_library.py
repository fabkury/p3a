# SPDX-License-Identifier: Apache-2.0
"""The library on the SD card: an index saved and loaded, and the blob store.

The logic of records and indices is host-tested (tests/host/library). These
tests are about the card: that what is saved is what is loaded, that a
damaged file is refused and left alone, and that blobs land where their
content says.
"""
import hashlib

import pytest

FORMAT_GIF, FORMAT_JPEG = 2, 4


def test_content(size: int, seed: int) -> bytes:
    """The bytes the device makes for `library blob <size> <seed>`."""
    return bytes((seed * 31 + i * 7 + (i >> 8)) & 0xFF for i in range(size))


test_content.__test__ = False  # a helper, not a test


@pytest.mark.parametrize("count", [2, 500, 4096])
def test_an_index_round_trips_on_the_card_and_damage_is_refused(uart, count):
    got = uart.command(f"library index {count}", timeout=90)
    assert got["missing"] == "MISSING", "no file yet: said so, not invented"
    assert got["round_trip"] is True, "same records, same order, each found by its identity"
    assert got["file_bytes"] == 32 + count * 192
    # Every third record has a blob; every eleventh is flagged gone and is not offered.
    assert got["available"] == sum(1 for n in range(count) if n % 3 == 0 and n % 11 != 0)
    assert got["too_many"] == "ESP_ERR_INVALID_SIZE", "a file larger than the caller's room is refused"
    assert got["flipped"] == "CORRUPT" and got["kept_after_flip"] is True and got["count_after_flip"] == 0
    assert got["bad_record"] == "CORRUPT", "intact checksums do not excuse a record that fails its own check"
    assert got["kept_after_bad"] is True and got["count_after_bad"] == 0, "none of its records is taken"
    if count == 4096:
        print(f"\n  4096 records: saved in {got['save_ms']} ms, loaded in {got['load_ms']} ms")
        assert got["load_ms"] < 3000 and got["save_ms"] < 5000


def test_a_blob_is_named_by_its_content_and_stored_once(uart):
    size, seed = 70_000, 11
    wanted = hashlib.sha256(test_content(size, seed)).digest()[:16].hex()
    uart.command(f"library blob-remove {wanted} {FORMAT_GIF}")  # from an earlier run, if any

    first = uart.command(f"library blob {size} {seed} {FORMAT_GIF}", timeout=30)
    assert first["result"] == "ok" and first["ref"] == wanted
    assert first["already"] is False and first["exists"] is True and first["stored_bytes"] == size
    assert "/blobs/" in first["path"] and first["path"].endswith(f"/{wanted}.gif")
    assert first["upload_left"] is False, "moved into the store, not copied"

    again = uart.command(f"library blob {size} {seed} {FORMAT_GIF}", timeout=30)
    assert again["result"] == "ok" and again["ref"] == wanted and again["path"] == first["path"]
    assert again["already"] is True and again["upload_left"] is False, "the second copy is dropped"

    other = uart.command(f"library blob {size} {seed + 1} {FORMAT_GIF}", timeout=30)
    assert other["ref"] == hashlib.sha256(test_content(size, seed + 1)).digest()[:16].hex() != wanted

    for ref in (wanted, other["ref"]):
        assert uart.command(f"library blob-remove {ref} {FORMAT_GIF}") == {"result": "ok", "exists": False}
    assert uart.command(f"library blob-remove {wanted} {FORMAT_GIF}")["result"] == "MISSING"


def test_what_is_not_a_blob_is_not_taken_in(uart):
    empty = uart.command(f"library blob 0 1 {FORMAT_JPEG}")
    assert empty["result"] == "CORRUPT" and empty["exists"] is False
    assert empty["upload_left"] is True, "a refused file is left where it was"
    for bad_format in (0, 6, 99):
        refused = uart.command(f"library blob 100 1 {bad_format}")
        assert refused["result"] == "ESP_ERR_INVALID_ARG" and refused["upload_left"] is True
    assert uart.command(f"library blob-remove {'00' * 16} {FORMAT_GIF}")["result"] == "ESP_ERR_INVALID_ARG"
