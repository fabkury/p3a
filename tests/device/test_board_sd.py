# SPDX-License-Identifier: Apache-2.0
"""The SD card mount and the SD fixes carried over from the jitter work."""
import pytest

import lab


def test_card_is_mounted_and_is_the_32_gb_one(uart):
    info = uart.command("sd info")
    assert info["mounted"] is True
    assert 28_000 < info["capacity_mb"] < 32_000, "the dev unit holds a 32 GB card"


def test_the_sd_fixes_are_linked_in(uart):
    info = uart.command("sd info")
    assert info["ff_aligned"] is True, "FatFS buffers must be cache-line aligned (no bounce buffers)"
    assert info["ff_psram"] is True, "and come from PSRAM, not from the internal DMA pool"
    assert info["idle_wait_wrap"] is True, "jitter fix 8: the yielding busy-wait replaces the IDF's"
    assert info["idf_patched"] is False, "IDF 5.5.4 does not carry Espressif's own fix; revisit if this flips"
    assert info["bus_locked"] is False


@pytest.mark.parametrize("size", [1, 511, 512, 4096, 65_536, 1_000_000])
def test_write_read_back_delete(uart, size):
    result = uart.command(f"sd rw {size}", timeout=30)
    assert result["bytes"] == size
    assert result["write_ms"] < 5000 and result["read_ms"] < 5000


def test_boot_mounts_the_card_without_complaint(uart):
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start)
    boot = uart.lines[start:]
    assert not any("boot: SD card failed" in line for line in boot)
    assert not any("not enough mem" in line for line in boot)
    assert uart.command("sd info")["mounted"] is True


def test_bad_sd_arguments(uart):
    for bad in ("sd", "sd rw", "sd rw 0", "sd rw 99999999", "sd nonsense"):
        with pytest.raises(lab.LabError):
            uart.command(bad)
