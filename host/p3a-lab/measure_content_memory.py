# SPDX-License-Identifier: Apache-2.0
"""Measures the internal memory pool while a Giphy channel is listed, downloaded and played.

    python host/p3a-lab/measure_content_memory.py <seconds>

Run from the repository root with the dev unit on a test build and the test rig's Giphy key in the
secrets file. 100 GIFs in the larger rendition, a new artwork every 5 seconds. Prints the pool's free
bytes as sampled every 2 seconds, and its lowest point since boot ("min_free ever"), which also sees
the dips between samples: start from a fresh boot for that figure to mean this run.

The internal pool is what the Wi-Fi link and the SD card allocate from; running it dry was 1.x's
crash (docs/known-issues.md, "SDIO RX"). First used for O9 (docs/2.0/DECISIONS.md D73).
"""
import sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "tests" / "device"))
import lab
from test_api import Api
from test_scheduler import Rig, LOCAL_ONLY

SECONDS = int(sys.argv[1])
uart, _ = lab.bring_up("test", do_flash=False)
rig = Rig(uart, Api(lab.provision(uart)["ip"]))
key = lab.load_secrets()["api_keys"]["giphy_api_key"]
uart.command("library evict 1000000000000 0 -", timeout=120)
uart.command("mem release")
idle = uart.command("mem")
uart.command("settings set", {"giphy_api_key": key, "giphy_cache_size": 100, "giphy_prefer_downsized": True,
                              "giphy_rating": "g", "dwell_s": 5})
uart.command("sched auto on")
uart.command("sched activate", {"name": "O9", "channels": [{"source": "giphy", "kind": "trending", "offset": int(time.time()) % 60}]})
t0 = time.monotonic()
samples = []
levels = set()
while time.monotonic() - t0 < SECONDS:
    m = uart.command("mem")
    samples.append((round(time.monotonic() - t0), m["free"], m["largest"], m["min_free"]))
    levels.add(m["level"])
    time.sleep(2)
s = rig.status()
last = uart.command("mem")
print("idle before:", {k: idle[k] for k in ("free", "largest", "min_free", "total")})
print("during: free min %d, median %d; largest block min %d; min_free ever %d; levels seen %s; pressure events %d" % (
    min(x[1] for x in samples), sorted(x[1] for x in samples)[len(samples) // 2], min(x[2] for x in samples),
    last["min_free"], sorted(levels), last["events"]))
print("playback:", {k: s[k] for k in ("shown", "failed", "downloads", "download_failures")},
      "channel", {k: s["channels"][0][k] for k in ("records", "available")})
log = rig.api.get("/api/v2/test/log").body
for needle in (b"sdio", b"SDIO", b"no mem", b"NO_MEM", b"rx buf", b"alloc"):
    print(needle.decode(), log.count(needle))
uart.command("sched auto off")
uart.command("sched activate", LOCAL_ONLY)
uart.command("settings set", {"giphy_api_key": "", "giphy_cache_size": 350, "giphy_rating": "pg-13", "dwell_s": 30})
uart.command("library evict 1000000000000 0 -", timeout=120)
uart.close()
