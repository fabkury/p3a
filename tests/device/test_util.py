# SPDX-License-Identifier: Apache-2.0
"""p3a_util on the device: what the host tests cannot see."""


def test_task_helper_applies_the_class_policy(uart):
    # The console task is created with class BACKGROUND through p3a_task_create.
    task = uart.command("task")
    assert task["name"] == "p3a_test_con"
    assert task["priority"] == 2
    assert task["core"] == 0
    assert task["psram_stack"] is True, "a BACKGROUND task's stack belongs in PSRAM"


def test_error_names_on_the_device(uart):
    base = 0x50000
    assert uart.command("errname 0x50001") == {"name": "NOT_PLAYABLE", "p3a": True}
    assert uart.command(f"errname {base + 7}") == {"name": "SD_FAILED", "p3a": True}
    # Outside the p3a range the IDF's own table answers.
    assert uart.command("errname 0x101") == {"name": "ESP_ERR_NO_MEM", "p3a": False}
    assert uart.command("errname 0") == {"name": "ESP_OK", "p3a": False}
