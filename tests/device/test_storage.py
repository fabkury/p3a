# SPDX-License-Identifier: Apache-2.0
"""The storage layer on the real card: paths, atomic writes, containers, health."""
import pytest

import lab

MASK = (1 << 64) - 1


def expected_shard(base: str, leaf: str, ext: str) -> str:
    """The shard layout, implemented independently of the firmware."""
    clean = "".join("_" if c in ':/\\?*"<>|' else c for c in leaf)
    h = 0xCBF29CE484222325
    for byte in clean.encode():
        h ^= byte
        h = (h * 0x100000001B3) & MASK
    h ^= h >> 33
    h = (h * 0xFF51AFD7ED558CCD) & MASK
    h ^= h >> 33
    h = (h * 0xC4CEB9FE1A85EC53) & MASK
    h ^= h >> 33
    return f"{base}/{h & 0x3F}/{(h >> 8) & 0x3F}/{clean}{ext}"


def test_storage_comes_into_service_at_boot(uart):
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    boot = uart.lines[start:]
    assert any("SD root: /sdcard/p3a" in line for line in boot)
    assert any("boot probe OK" in line for line in boot)
    assert not any("boot: storage failed" in line for line in boot)

    info = uart.command("storage info")
    assert info["root"] == "/sdcard/p3a", "the default root until settings exist"
    assert info["health"] == "ok"
    assert info["fat_check_ran"] is True, "the FAT mirror check runs right after the mount"
    assert info["fat_sectors"] > 1000
    assert info["fat_mismatched"] == 0 and info["fat_restored"] == 0, "a healthy card needs no repair"
    assert info["repairs_boot"] == 0


def test_free_space_is_measured_off_the_boot_path_and_cached(uart):
    import time
    deadline = time.monotonic() + 30
    info = uart.command("storage info")
    while not info["space_valid"] and time.monotonic() < deadline:
        time.sleep(0.5)
        info = uart.command("storage info")
    assert info["space_valid"] is True, "the boot job measures free space"
    assert 28_000 < info["total_mb"] < 32_000
    assert 0 < info["free_mb"] <= info["total_mb"]

    measured = uart.command("storage space", timeout=60)
    assert measured["total_mb"] == info["total_mb"]
    # Reading the cached figure is instant however long measuring took.
    again = uart.command("storage info")
    assert again["space_age_ms"] < 5000 and again["free_mb"] == measured["free_mb"]


@pytest.mark.parametrize("size", [1, 4096, 300_000])
def test_atomic_write_replaces_and_leaves_no_temporary_file(uart, size):
    result = uart.command(f"storage atomic atomic-test.bin {size}", timeout=30)
    assert result == {"bytes": size, "tmp_left": False}


@pytest.mark.parametrize("count", [0, 1, 2048])
def test_container_round_trip_and_damage_reports(uart, count):
    r = uart.command(f"storage frame {count}", timeout=30)
    assert r["round_trip"] is True
    assert r["flags"] == 1, "the rebuildable flag survives the round trip"
    assert r["missing"] == "MISSING", "no file is its own error, not a generic failure"
    assert r["other_version"] == "ESP_ERR_INVALID_VERSION", "an intact file of another version is not damage"
    assert r["flipped"] == "CORRUPT"
    assert r["kept_after_flip"] is True, "a file that fails its check is never deleted"
    if count:
        assert r["truncated"] == "ESP_ERR_INVALID_SIZE"
        assert r["kept_after_truncate"] is True


@pytest.mark.parametrize("leaf,ext", [
    ("abc", ".webp"),
    ("3f2b8c1e-7d4a-4e0b-9c1d-5a6b7c8d9e0f", ".gif"),
    ("urn-3:HUAM:12345_dynmc", ".jpg"),   # colons must be sanitized before hashing
    ("no-extension", ""),
])
def test_shard_layout_matches_an_independent_implementation(uart, leaf, ext):
    command = f"storage shard {leaf} {ext}".rstrip()
    assert uart.command(command)["path"] == expected_shard("/sdcard/p3a/shard-test", leaf, ext)


def test_missing_parent_folders_are_created(uart):
    assert uart.command("storage parents parents-test/a/b/c/file.bin") == {"created": True}


def test_health_latch_trips_after_three_failures_and_stays_until_reboot(uart):
    before = uart.command("storage info")
    assert before["health"] == "ok"
    try:
        r = uart.command("storage latch")
        assert r["failed_after_disk_full"] is False, "a full card is not a failed card"
        assert r["failed"] is True and r["health"] == "failed"
        assert r["events"] == before["health_events"] + 1, "the failure is announced once"
        assert r["write_after"] == "SD_FAILED", "nothing writes to a card that has failed"

        # More failures neither re-announce nor clear it; a container save is refused too.
        again = uart.command("storage latch")
        assert again["events"] == r["events"]
        with pytest.raises(lab.LabError, match="SD_FAILED"):
            uart.command("storage frame 4")
    finally:
        start = uart.mark()
        uart.command("reboot")
        uart.wait_ready(since=start, timeout=30)
    assert uart.command("storage info")["health"] == "ok", "only a reboot clears the latch"


def test_bad_storage_arguments(uart):
    for bad in ("storage", "storage atomic x", "storage atomic x 0", "storage frame", "storage frame -1",
                "storage nonsense"):
        with pytest.raises(lab.LabError):
            uart.command(bad)
