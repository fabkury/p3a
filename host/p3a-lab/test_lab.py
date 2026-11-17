# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the parts of lab.py that need no device.

    python -m pytest host/p3a-lab/test_lab.py
"""
import lab


def test_ordinary_boot_log_has_no_panics():
    log = [
        "ESP-ROM:esp32p4-eco2-20240710",
        "rst:0x1 (POWERON),boot:0x30f (SPI_FAST_FLASH_BOOT)",
        "I (26) boot: ESP-IDF v5.5.4 2nd stage bootloader",
        "I (1016) esp_core_dump_flash: Found partition 'coredump' @ 220000 262144 bytes",
        "W (1047) p3a: TEST BUILD: test hooks are compiled in. Not for release.",
        "I (1062) p3a: boot complete",
        "rst:0xc (SW_CPU_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)",  # esp_restart(), deliberate
    ]
    assert lab.find_panics(log) == []


def test_crash_signatures_are_found():
    crashes = [
        "Guru Meditation Error: Core  0 panic'ed (Load access fault). Exception was unhandled.",
        "abort() was called at PC 0x4ff0abcd on core 1",
        "assert failed: sdio_rx_get_buffer sdio_drv.c:812 (*buf)",
        "E (30123) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:",
        "Guru Meditation Error: Core  1 panic'ed (Interrupt wdt timeout on CPU1).",
        "***ERROR*** A stack overflow in task anim_loader has been detected.",
        "rst:0x7 (TG0_WDT_HPSYS),boot:0x30f (SPI_FAST_FLASH_BOOT)",
        "rst:0xf (HP_SYS_HP_WDT_RESET),boot:0x30f (SPI_FAST_FLASH_BOOT)",
        "Rebooting...",
    ]
    for line in crashes:
        assert lab.find_panics(["I (1) ok", line]) == [line], line


def test_secret_values_skips_metadata_and_short_strings():
    secrets = {
        "_comment": "never redact this sentence",
        "wifi": {"ssid": "MyNetwork", "password": "hunter2hunter2"},
        "api_keys": {"giphy_api_key": "g" * 32, "empty": "", "short": "abc"},
        "captured": {"from": "p3a-fab.local /config"},
    }
    assert sorted(lab.secret_values(secrets)) == sorted(["hunter2hunter2", "g" * 32])


def test_project_version_reads_the_root_cmake():
    assert lab.project_version().count(".") == 2
