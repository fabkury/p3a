# SPDX-License-Identifier: Apache-2.0
"""The link to the ESP32-C6 Wi-Fi co-processor and the firmware p3a carries for it.

The dev unit's C6 runs esp_hosted 2.7.0, the one version whose own bug makes
it impossible to update (esp-hosted-mcu issue 143). So on this unit the update
itself can never run; these tests cover the link, the version exchange, the
lock detection, and the image that an update would send.
"""


def test_link_comes_up_and_the_c6_reports_its_version(uart):
    c6 = uart.command("c6")
    assert c6["running_known"] is True, "the co-processor answered over SDIO"
    assert c6["running"] == "2.7.0", "the dev unit's C6 (change this if the C6 is ever reflashed)"
    assert c6["updating"] is False


def test_the_unupdatable_version_is_recognised(uart):
    assert uart.command("c6")["locked"] is True


def test_the_image_an_update_would_send_is_in_the_slave_fw_partition(uart):
    c6 = uart.command("c6")
    assert c6["embedded"] == "2.9.3"
    assert c6["partition"] is True
    assert c6["image_ok"] is True, "slave_fw holds a valid application image"
    assert c6["image_project"] == "network_adapter"


def test_boot_checks_the_co_processor_and_carries_on(uart):
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start, timeout=30)
    boot = uart.lines[start:]
    assert any("Current co-processor firmware: 2.7.0" in line for line in boot)
    assert any("OTA not possible" in line for line in boot)
    assert not any("boot: co-processor failed" in line for line in boot)
    assert any("boot complete" in line for line in boot)
