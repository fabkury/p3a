# SPDX-License-Identifier: Apache-2.0
"""The job runners on the device (the job table itself is host-tested)."""
import time

import pytest

import lab


def wait_done(uart, job_id, timeout=5.0):
    """Polls a job until it is done; returns every snapshot seen on the way."""
    seen = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = uart.command(f"jobinfo {job_id}")
        seen.append(info)
        if info["state"] == "done":
            return seen
        time.sleep(0.02)
    pytest.fail(f"job {job_id} did not finish in {timeout}s; last: {seen[-1] if seen else None}")


@pytest.mark.parametrize("runner", ["io", "ui"])
def test_a_job_runs_and_reports_progress(uart, runner):
    job_id = uart.command(f"job ok {runner}")["id"]
    seen = wait_done(uart, job_id)
    final = seen[-1]
    assert final["name"] == "test_ok"
    assert final["result"] == 0 and final["result_name"] == "ESP_OK"
    assert final["progress"] == 100
    progress = [s["progress"] for s in seen]
    assert progress == sorted(progress), "progress never goes backwards"
    assert any(s["state"] == "running" and 0 < s["progress"] < 100 for s in seen), \
        "progress should be visible while the job runs"


def test_a_failing_job_keeps_its_error(uart):
    job_id = uart.command("job fail")["id"]
    final = wait_done(uart, job_id)[-1]
    assert final["result_name"] == "CORRUPT"
    assert final["result"] == 0x50002
    assert final["progress"] == 30, "a failed job keeps the progress it reached"


def test_flash_write_from_a_runner_task(uart):
    # Runner stacks are in PSRAM. An NVS write from one must work.
    first = wait_done(uart, uart.command("job nvs")["id"], timeout=10)[-1]
    assert first["result_name"] == "ESP_OK", first
    second = wait_done(uart, uart.command("job nvs")["id"], timeout=10)[-1]
    assert second["result_name"] == "ESP_OK", second


def test_jobs_on_one_runner_run_in_order_and_runners_are_independent(uart):
    slow = uart.command("job slow io")["id"]
    behind = uart.command("job ok io")["id"]
    other = uart.command("job ok ui")["id"]

    # The other runner is not held up by the slow job.
    wait_done(uart, other, timeout=1.2)
    assert uart.command(f"jobinfo {slow}")["state"] == "running"
    assert uart.command(f"jobinfo {behind}")["state"] == "queued"

    wait_done(uart, behind, timeout=5)
    assert uart.command(f"jobinfo {slow}")["state"] == "done"


def test_a_full_queue_refuses_and_recovers(uart):
    # One slow job running plus eight queued fills the I/O runner.
    ids = [uart.command("job slow io")["id"]]
    ids += [uart.command("job ok io")["id"] for _ in range(8)]
    with pytest.raises(lab.LabError, match="ESP_ERR_NO_MEM"):
        uart.command("job ok io")
    for job_id in ids:
        assert wait_done(uart, job_id, timeout=8)[-1]["result_name"] == "ESP_OK"
    # And it accepts work again.
    assert wait_done(uart, uart.command("job ok io")["id"])[-1]["result_name"] == "ESP_OK"


def test_unknown_job(uart):
    with pytest.raises(lab.LabError, match="unknown job"):
        uart.command("jobinfo 999999")
    with pytest.raises(lab.LabError, match="unknown job kind"):
        uart.command("job nonsense")
