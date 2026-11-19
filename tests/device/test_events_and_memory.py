# SPDX-License-Identifier: Apache-2.0
"""The event bus and the memory-pressure signal on the device."""
import pytest

import lab


def test_fanout_reaches_every_subscriber_in_order(uart):
    r = uart.command("event fanout 20")
    assert r["accepted"] == 20 and r["dropped"] == 0
    assert r["first"] == 20 and r["second"] == 20, "both subscribers get every event"
    assert r["order_errors"] == 0, "events arrive in emit order, subscribers run in subscription order"
    assert r["relayed"] == 20, "an event emitted from inside a handler is delivered"
    assert r["once"] == 1, "a handler can unsubscribe itself without deadlocking the bus"


def test_fanout_larger_than_the_queue(uart):
    # 200 events through a 32-slot queue: the dispatcher outranks the emitter,
    # so nothing is dropped, and each handler also emits a second event.
    r = uart.command("event fanout 200", timeout=10)
    assert r["accepted"] == 200 and r["dropped"] == 0
    assert r["first"] == 200 and r["relayed"] == 200 and r["order_errors"] == 0


def test_a_full_queue_drops_and_says_so(uart):
    start = uart.mark()
    r = uart.command("event flood 60", timeout=15)
    assert r["dropped"] > 0, "a slow handler and 60 events must overflow a 32-slot queue"
    assert r["accepted"] + r["dropped"] == 60, "every emit either queued or was reported dropped"
    assert r["slow"] == r["accepted"], "every accepted event was still delivered"
    assert any("queue full" in line for line in uart.lines[start:]), "the first drop is logged"

    # The bus works normally afterwards.
    again = uart.command("event fanout 5")
    assert again["first"] == 5 and again["dropped"] == 0


def test_memory_pressure_levels_follow_the_pool(uart):
    try:
        base = uart.command("mem release")
        assert base["level"] == "ok", base
        # About 447 KB while the firmware is nearly empty; 1.x, fully loaded,
        # left about 272 KB. It shrinks as components arrive.
        assert 150_000 < base["total"] < 600_000, "the internal DMA pool is a few hundred KB"
        events = base["events"]

        # Take the pool down to about 52 KB free: below the 64 KB LOW threshold.
        low = uart.command(f"mem hog {base['free'] - 52_000}")
        assert low["level"] == "low", low
        assert low["events"] == events + 1 and low["last_event_level"] == 1

        # Down to about 30 KB: below the 40 KB CRITICAL threshold.
        critical = uart.command(f"mem hog {low['free'] - 30_000}")
        assert critical["level"] == "critical", critical
        assert critical["events"] == events + 2 and critical["last_event_level"] == 2
        assert critical["min_free"] <= critical["free"]
    finally:
        after = uart.command("mem release")

    assert after["level"] == "ok", after
    assert after["hogged"] == 0
    assert after["events"] == events + 3 and after["last_event_level"] == 0
    assert after["free"] > base["free"] - 4096, "the pool is back to where it started"


def test_bad_arguments(uart):
    import lab
    with pytest.raises(lab.LabError, match="usage"):
        uart.command("event fanout 0")
    with pytest.raises(lab.LabError, match="usage"):
        uart.command("mem nonsense")


@pytest.mark.slow
def test_first_failed_allocation_prints_one_heap_report(uart):
    # With the Wi-Fi station off: it allocates and frees in the same pool, so
    # with it running neither "who failed first" nor "what is left" is fixed.
    # (What exhaustion does to a live network is a soak question, M6.1.)
    uart.command("net forget")
    # Fresh boot, because the report is printed once per boot.
    start = uart.mark()
    uart.command("reboot")
    uart.wait_ready(since=start)
    assert uart.command("net status")["state"] == "setup"
    start = uart.mark()
    try:
        # Ask for more than the pool holds: the hog stops at the first failure.
        exhausted = uart.command("mem hog 600000", timeout=10)
        assert exhausted["level"] == "critical"
        # Some bytes stay free in fragments; what matters is that no chunk fits.
        # A free block of exactly the chunk's size does not fit one either: the
        # allocator needs room for its own header beside it.
        assert exhausted["largest"] <= 2048
        # Fail again; the report must not repeat.
        uart.command("mem hog 600000", timeout=10)
    finally:
        after = uart.command("mem release")
    log = uart.lines[start:]
    reports = [line for line in log if "HEAP ALLOC FAILED" in line]
    assert len(reports) == 1, "exactly one report per boot"
    assert any("task=p3a_test_con" in line and "size=2048" in line for line in log), \
        "the report names the request and the task that made it"
    assert any("INTERNAL+DMA" in line for line in log)
    assert after["level"] == "ok", "the device recovers once the memory is returned"
    lab.provision(uart)
