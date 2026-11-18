# SPDX-License-Identifier: Apache-2.0
"""The settings store on the real device: NVS, reboots, factory reset, recovery counters.

Every test starts from a factory reset and the module ends with one, so the
dev unit is left with default settings.
"""
import json
from pathlib import Path

import pytest

import lab

SCHEMA = json.loads(
    (Path(__file__).resolve().parents[2] / "components" / "p3a_settings" / "settings.json").read_text(encoding="utf-8")
)["settings"]
DEFAULTS = {s["key"]: s["default"] for s in SCHEMA}


def reboot(uart) -> list[str]:
    """Restarts the device and returns the boot log."""
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    return uart.lines[start:]


def factory_reset(uart) -> list[str]:
    uart.command("settings factory-reset")
    return reboot(uart)


def set_settings(uart, values: dict) -> dict:
    return uart.command("settings set " + json.dumps(values, separators=(",", ":")))


@pytest.fixture
def fresh(uart):
    factory_reset(uart)
    yield uart


@pytest.fixture(scope="module", autouse=True)
def leave_defaults(bring_up):
    yield
    factory_reset(bring_up[0])


def test_a_fresh_device_holds_the_schema_defaults(fresh):
    info = fresh.command("settings info")
    assert info["count"] == len(SCHEMA)
    assert info["loaded"] is False and info["blob_unreadable"] is False
    assert info["stored_bytes"] == 0
    assert fresh.command("settings get") == DEFAULTS


def test_settings_survive_a_reboot(fresh):
    values = {
        "brightness": 35,
        "rotation": 180,
        "background_color": "#102030",
        "show_fps": True,
        "pick_mode": "recency",
        "giphy_country": "br",
        "dwell_s": 0,
        "museum_refresh_s": 691200,
    }
    reply = set_settings(fresh, values)
    assert sorted(reply["changed"]) == sorted(values)
    assert reply["reboot_needed"] is False

    boot = reboot(fresh)
    assert any("settings: loaded" in line for line in boot)
    assert not any("boot: settings failed" in line for line in boot)
    now = fresh.command("settings get")
    assert now == {**DEFAULTS, **values, "giphy_country": "BR"}
    info = fresh.command("settings info")
    assert info["loaded"] is True and info["skipped"] == 0
    assert 0 < info["stored_bytes"] < 400, "only values that differ from the defaults are stored"


def test_boot_applies_brightness_and_background(fresh):
    set_settings(fresh, {"brightness": 20, "background_color": "#203040"})
    reboot(fresh)
    assert fresh.command("lcd info")["brightness"] == 20
    # Back to the default: nothing is stored, and the backlight is at full again.
    set_settings(fresh, {"brightness": 100})
    reboot(fresh)
    assert fresh.command("lcd info")["brightness"] == 100


def test_a_refused_request_changes_nothing(fresh):
    for bad, expect in (
        ({"brightness": 50, "rotation": 45}, "rotation"),
        ({"brightness": 50, "no_such_setting": 1}, "no_such_setting is not a setting"),
        ({"brightness": 101}, "brightness is out of range"),
        ({"device_name": "Has Spaces"}, "device_name"),
        ({"dwell_time_ms": 30000}, "is not a setting"),
    ):
        with pytest.raises(lab.LabError) as caught:
            set_settings(fresh, bad)
        assert expect in str(caught.value)
    assert fresh.command("settings get") == DEFAULTS
    info = fresh.command("settings info")
    assert info["events"] == 0 and info["stored_bytes"] == 0


def test_one_event_per_changed_setting_and_none_for_the_same_value(fresh):
    assert fresh.command("settings info")["events"] == 0
    assert set_settings(fresh, {"brightness": 60, "show_fps": True})["changed"] == ["brightness", "show_fps"]
    assert fresh.command("settings info")["events"] == 2
    assert set_settings(fresh, {"brightness": 60, "show_fps": True})["changed"] == []
    assert fresh.command("settings info")["events"] == 2
    assert set_settings(fresh, {"brightness": 61, "show_fps": True})["changed"] == ["brightness"]
    info = fresh.command("settings info")
    assert info["events"] == 3 and info["last_changed"] == "brightness"


def test_typed_setters(fresh):
    fresh.command("settings typed indicator_size int 128")
    fresh.command("settings typed playback_max_speed bool true")
    fresh.command("settings typed klipy_rating str g")
    fresh.command("settings typed background_color str #ff0000")
    now = fresh.command("settings get")
    assert now["indicator_size"] == 128 and now["playback_max_speed"] is True
    assert now["klipy_rating"] == "g" and now["background_color"] == "#ff0000"
    with pytest.raises(lab.LabError):
        fresh.command("settings typed indicator_size int 4")
    with pytest.raises(lab.LabError):
        fresh.command("settings typed no_such_key int 4")


def test_reboot_flag_is_raised_only_by_settings_that_need_one(fresh):
    assert set_settings(fresh, {"brightness": 50})["reboot_needed"] is False
    assert set_settings(fresh, {"device_name": "bench"})["reboot_needed"] is True
    assert fresh.command("settings info")["reboot_needed"] is True
    reboot(fresh)
    assert fresh.command("settings info")["reboot_needed"] is False
    assert fresh.command("settings get")["device_name"] == "bench"


def test_a_secret_is_stored_but_never_shown_or_logged(fresh):
    secret = "test-key-0123456789abcdef"
    start = fresh.mark()
    assert set_settings(fresh, {"giphy_api_key": secret})["changed"] == ["giphy_api_key"]
    shown = fresh.command("settings get")
    assert shown["giphy_api_key"] == "********"
    assert shown["klipy_api_key"] == "", "an unset secret reads as empty"
    # Sending the whole page back, mask included, keeps the secret.
    assert set_settings(fresh, shown)["changed"] == []
    boot = reboot(fresh)
    assert fresh.command("settings get")["giphy_api_key"] == "********"
    # The value appears once on the wire, in the command the test itself sent
    # (the console echoes nothing), and never in what the device printed.
    # The device never prints the value. (Lines the lab writes itself, such
    # as the command it sent, are marked "[lab]".)
    device_output = [line for line in fresh.lines[start:] + boot if "[lab]" not in line]
    assert not any(secret in line for line in device_output)
    # Still set: clearing it is a change.
    assert set_settings(fresh, {"giphy_api_key": ""})["changed"] == ["giphy_api_key"]


def test_the_sd_root_setting_moves_the_root_at_the_next_boot(fresh):
    assert fresh.command("storage info")["root"] == "/sdcard/p3a"
    assert set_settings(fresh, {"sd_root": "/p3a-test-root"})["reboot_needed"] is True
    assert fresh.command("storage info")["root"] == "/sdcard/p3a", "not before a reboot"
    boot = reboot(fresh)
    assert any("SD root: /sdcard/p3a-test-root" in line for line in boot)
    info = fresh.command("storage info")
    assert info["root"] == "/sdcard/p3a-test-root" and info["health"] == "ok"
    factory_reset(fresh)
    assert fresh.command("storage info")["root"] == "/sdcard/p3a"


def test_stored_entries_this_firmware_does_not_accept_are_skipped(fresh):
    fresh.command('settings plant {"brightness":45,"gone_in_this_version":1,"rotation":45,"pick_mode":"recency"}')
    boot = reboot(fresh)
    assert any("2 stored entries ignored" in line for line in boot)
    now = fresh.command("settings get")
    assert now["brightness"] == 45 and now["pick_mode"] == "recency" and now["rotation"] == 0
    info = fresh.command("settings info")
    assert info["loaded"] is True and info["skipped"] == 2 and info["blob_unreadable"] is False


def test_an_unreadable_blob_is_kept_aside_and_defaults_are_used(fresh):
    garbage = '{"brightness":45,'
    fresh.command("settings plant " + garbage)
    boot = reboot(fresh)
    assert any("stored settings do not parse" in line for line in boot)
    assert fresh.command("settings get") == DEFAULTS
    assert fresh.command("settings info")["blob_unreadable"] is True
    assert fresh.command("settings aside")["bytes"] == len(garbage)
    # The store still works, and the next boot is clean.
    set_settings(fresh, {"brightness": 70})
    reboot(fresh)
    assert fresh.command("settings get")["brightness"] == 70
    assert fresh.command("settings info")["blob_unreadable"] is False


def test_factory_reset_returns_everything_to_defaults(fresh):
    set_settings(fresh, {"brightness": 10, "device_name": "gone-soon", "harvard_api_key": "abc"})
    fresh.command("recovery wifi fail 3")
    fresh.command("settings factory-reset")
    # Between the reset and the restart the store refuses changes.
    with pytest.raises(lab.LabError) as caught:
        set_settings(fresh, {"brightness": 11})
    assert "ESP_ERR_INVALID_STATE" in str(caught.value)
    boot = reboot(fresh)
    assert any("nothing stored; using defaults" in line for line in boot)
    assert fresh.command("settings get") == DEFAULTS
    assert fresh.command("recovery wifi") == {"action": "", "total": 0, "streak": 0}


def test_recovery_counters_bound_the_restarts_and_persist(fresh):
    assert fresh.command("recovery wifi") == {"action": "", "total": 0, "streak": 0}
    assert fresh.command("recovery wifi fail 2") == {"action": "reboot", "total": 1, "streak": 1}
    reboot(fresh)
    assert fresh.command("recovery wifi fail 2") == {"action": "reboot", "total": 2, "streak": 2}
    reboot(fresh)
    # The allowance is used up: carry on without it, and count nothing more.
    assert fresh.command("recovery wifi fail 2") == {"action": "degraded", "total": 2, "streak": 2}
    # Success clears the streak and keeps the total.
    assert fresh.command("recovery wifi ok 2") == {"action": "continue", "total": 2, "streak": 0}
    assert fresh.command("recovery wifi fail 2") == {"action": "reboot", "total": 3, "streak": 1}
    # Kinds are independent.
    assert fresh.command("recovery link") == {"action": "", "total": 0, "streak": 0}


def test_a_working_touch_controller_leaves_its_counters_alone(fresh):
    boot = reboot(fresh)
    assert not any("restarting to retry the touch controller" in line for line in boot)
    assert fresh.command("recovery touch") == {"action": "", "total": 0, "streak": 0}
