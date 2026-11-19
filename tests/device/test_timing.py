# SPDX-License-Identifier: Apache-2.0
"""Frame timing, read from the frame trace: how late frames reach the panel
while animations play and while the next artwork loads.

Needs the build with the frame trace and the test hooks:

    python -m pytest tests/device/test_timing.py --build trace-test -s

On any other build these tests are skipped. They are a short baseline, not
the soak: the pass bar of the jitter work (docs/jitter/REPORT.md) is no frame
100 ms late or more over hours, and minutes cannot show that. They catch a
pipeline that is broken, not one that is rarely late.
"""
import csv
import io
import statistics
import time

import pytest
from PIL import Image

import lab
from test_api import Api
from test_decoders import CORPUS

pytestmark = [pytest.mark.live, pytest.mark.slow]

LATE_US = 100_000  # the jitter work's bar for one frame


@pytest.fixture(scope="module")
def api(bring_up):
    client = Api(lab.provision(bring_up[0])["ip"])
    if client.get("/api/v2/debug/frames/stats").status != 200:
        pytest.skip("this build has no frame trace; use --build trace-test")
    client.call("PATCH", "/api/v2/settings", {"rotation": 0, "background_color": "#000000", "show_fps": False,
                                                 "playback_max_speed": False})
    yield client
    client.call("POST", "/api/v2/test/play/stop")


def animation(size: int, frames: int, delay_ms: int, fmt: str) -> bytes:
    """A moving gradient: every frame differs everywhere, so nothing is cheap to decode."""
    images = []
    for n in range(frames):
        img = Image.new("RGB", (size, size))
        img.putdata([((x * 255 // size + n * 16) & 255, (y * 255 // size + n * 9) & 255, (x + y + n * 5) & 255)
                     for y in range(size) for x in range(size)])
        images.append(img)
    out = io.BytesIO()
    if fmt == "gif":
        images[0].save(out, "GIF", save_all=True, append_images=images[1:], duration=delay_ms, loop=0)
    else:
        images[0].save(out, "WEBP", save_all=True, append_images=images[1:], duration=delay_ms, loop=0,
                       lossless=True)
    return out.getvalue()


def play(api: Api, file: bytes, smooth: bool = False) -> dict:
    data = api.call("POST", f"/api/v2/test/play?smooth={int(smooth)}", file, timeout=90).data
    assert data["outcome"] == "shown", data
    return data


def frames_since(api: Api, seq: int) -> tuple[list[dict], list[dict], int]:
    """The trace's frame rows and mark rows from `seq` on, and where to continue."""
    text = api.get(f"/api/v2/debug/frames?since={seq}", timeout=60).body.decode()
    lines = text.splitlines()
    assert lines[-1].startswith("#next,"), lines[-1][:80]
    rows = list(csv.DictReader(lines[:-1]))
    return ([r for r in rows if r["type"] == "F"], [r for r in rows if r["type"] == "M"], int(lines[-1].split(",")[1]))


def measure(api: Api, seconds: float, during=None) -> dict:
    start = api.get("/api/v2/debug/frames/stats").data["next_seq"]
    began = time.monotonic()
    if during:
        during()
    time.sleep(max(0.0, seconds - (time.monotonic() - began)))
    frames, marks, _ = frames_since(api, start)
    late = sorted(int(f["lateness_us"]) for f in frames)
    produce = sorted(int(f["produce_us"]) for f in frames)
    assert late, "no frames were presented"
    return {
        "frames": len(frames),
        "seconds": time.monotonic() - began,
        "late_p50_ms": statistics.median(late) / 1000,
        "late_p99_ms": late[int(len(late) * 0.99)] / 1000 if len(late) > 1 else late[0] / 1000,
        "late_max_ms": late[-1] / 1000,
        "over_bar": sum(1 for v in late if v >= LATE_US),
        "produce_p50_ms": statistics.median(produce) / 1000,
        "produce_max_ms": produce[-1] / 1000,
        "loads": sum(1 for m in marks if m["kind"] == "loader_load" and m["phase"] == "1"),
    }


def show(label: str, m: dict) -> None:
    print(f"\n  {label}: {m['frames']} frames in {m['seconds']:.0f} s; late p50 {m['late_p50_ms']:.2f} "
          f"p99 {m['late_p99_ms']:.2f} max {m['late_max_ms']:.2f} ms; produce p50 {m['produce_p50_ms']:.1f} "
          f"max {m['produce_max_ms']:.1f} ms; loads {m['loads']}")


def test_the_trace_reads_out_and_continues_where_it_left_off(api):
    play(api, (CORPUS / "good" / "gif" / "opaque_anim.gif").read_bytes())
    stats = api.get("/api/v2/debug/frames/stats").data
    assert stats["config"]["entries"] >= 8192 and stats["next_seq"] >= stats["oldest_seq"]
    assert api.call("POST", "/api/v2/debug/mark?arg=4242").status == 200
    time.sleep(1.0)
    frames, marks, next_seq = frames_since(api, stats["next_seq"])
    assert len(frames) >= 5, "an animation is playing"
    assert any(m["kind"] == "user" and m["arg"] == "4242" for m in marks)
    assert all(int(a["seq"]) < int(b["seq"]) for a, b in zip(frames, frames[1:]))
    again, _, _ = frames_since(api, next_seq)
    assert all(int(f["seq"]) >= next_seq for f in again)


@pytest.mark.parametrize("label,size,frames,delay_ms,fmt,smooth", [
    ("small GIF, 10 fps, nearest", 64, 12, 100, "gif", False),
    ("240 px GIF, 25 fps, nearest", 240, 16, 40, "gif", False),
    # Lossless WebP of this picture takes about 47 ms a frame to decode and scale:
    # 25 fps is beyond the hardware, in 1.x as here. 12 fps is not.
    ("240 px lossless WebP, 12 fps, nearest", 240, 16, 80, "webp", False),
    ("240 px GIF, 25 fps, smooth", 240, 16, 40, "gif", True),
])
def test_an_animation_plays_without_late_frames(api, label, size, frames, delay_ms, fmt, smooth):
    play(api, animation(size, frames, delay_ms, fmt), smooth)
    time.sleep(1.0)  # the first second holds the swap itself
    m = measure(api, 20.0)
    show(label, m)
    wanted = m["seconds"] * 1000 / delay_ms
    assert m["frames"] >= 0.9 * wanted, f"{m['frames']} frames where about {wanted:.0f} were due"
    assert m["over_bar"] == 0, f"{m['over_bar']} frames were 100 ms late or more"


def test_loading_the_next_artwork_does_not_hold_up_the_one_playing(api):
    playing = animation(240, 16, 40, "gif")
    big = io.BytesIO()
    Image.effect_noise((700, 700), 64).convert("RGB").save(big, "PNG")  # about 1 MB, slow to decode
    doomed = (CORPUS / "bad" / "png" / "png_truncated_half.png").read_bytes()
    play(api, playing)
    time.sleep(1.0)

    def loads():
        # Loads that end in a failure: the animation stays on the panel
        # throughout, so every frame in the window is one of its own.
        for _ in range(4):
            api.call("POST", "/api/v2/test/play", doomed, timeout=60)
            api.call("POST", "/api/v2/test/play?path=%2Fsdcard%2Fno%2Fsuch.gif", timeout=60)

    m = measure(api, 15.0, during=loads)
    show("240 px GIF with 8 failed loads", m)
    assert m["over_bar"] == 0, f"{m['over_bar']} frames were 100 ms late or more during loads"

    def swaps():
        for _ in range(3):
            play(api, big.getvalue())
            play(api, playing)

    m = measure(api, 20.0, during=swaps)
    show("6 swaps between a 700 px PNG and the GIF", m)
    assert m["loads"] >= 6
    # A swap restarts the playhead, so lateness across one is not a late
    # frame of an animation; the bar here is that nothing stalls for long.
    assert m["late_max_ms"] < 500, "a swap held the panel for half a second"
