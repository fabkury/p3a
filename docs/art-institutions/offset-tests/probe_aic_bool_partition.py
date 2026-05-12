"""
Verify the bool-filter + range partitioning escape hatch on AIC's
/artworks/search.

The earlier test established that POST with body:
  {"query": {"bool": {"must": [{"term": {"artwork_type_id": 1}}],
                       "filter": [{"range": {"id": {"gte": X, "lt": Y}}}]}},
   "from": F, "size": S, "fields": ["id"]}
works AND combines a facet filter with a range filter. The 1000-cap
still applies (from + size <= 1000) but now per-bucket — if a bucket
has < 1000 records we can fully traverse it.

This script:
  1. Probes Painting's ID distribution to find natural bucket
     boundaries with <1000 records each.
  2. Sums per-bucket counts to confirm we cover all 3794 paintings.
  3. Picks two buckets and exhaustively pages through both.
  4. Demonstrates a deep-offset query within one bucket
     (e.g. bucket has 800 records → from=700 size=100 should work).

Also tries OAI-PMH at several known patterns (the previous probe
errored out before testing them).
"""

import json
import time
import urllib.request
import urllib.parse

ART_BASE = "https://api.artic.edu/api/v1"
USER_AGENT = "p3a-offset-probe/1.0 (pub@kury.dev)"


def post(url: str, body: dict) -> tuple[int, dict | None, str]:
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        method="POST",
    )
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", "application/json")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            data = json.loads(r.read())
            return r.status, data, ""
    except urllib.error.HTTPError as e:
        body_bytes = e.read()
        return e.code, None, body_bytes[:300].decode("utf-8", errors="replace")


def get(url: str, accept="application/json") -> tuple[int, str, str]:
    req = urllib.request.Request(url)
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", accept)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8", errors="replace")[:500], dict(r.headers).get("Content-Type", "")
    except urllib.error.HTTPError as e:
        return e.code, e.read()[:200].decode(errors="replace"), ""
    except Exception as e:
        return -1, str(e)[:200], ""


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def bucket_count(low: int, high: int) -> int:
    """Count Paintings with id in [low, high)."""
    body = {
        "query": {
            "bool": {
                "must": [{"term": {"artwork_type_id": 1}}],
                "filter": [{"range": {"id": {"gte": low, "lt": high}}}],
            }
        },
        "size": 0,
        "fields": ["id"],
    }
    status, data, err = post(ART_BASE + "/artworks/search", body)
    if status != 200 or not data:
        print(f"    bucket [{low}..{high}) failed: status={status} err={err[:120]}")
        return -1
    return data.get("pagination", {}).get("total", -1)


def main() -> None:
    section("Step 1 — Painting ID distribution (split [0..300000) into 6 buckets)")
    boundaries = [0, 10000, 30000, 60000, 100000, 150000, 200000, 300000]
    counts = []
    cumulative = 0
    for i in range(len(boundaries) - 1):
        low, high = boundaries[i], boundaries[i + 1]
        n = bucket_count(low, high)
        if n >= 0:
            counts.append((low, high, n))
            cumulative += n
        print(f"    bucket [{low:>6}..{high:<6}): {n} paintings")
        time.sleep(1.0)
    print(f"  Sum across buckets: {cumulative}; expected total: 3794")

    section("Step 2 — Pick the largest bucket and traverse fully")
    if not counts:
        print("  no buckets to traverse")
    else:
        largest = max(counts, key=lambda c: c[2])
        low, high, n = largest
        if n > 1000:
            print(f"  largest bucket [{low}..{high}) has {n} records — STILL exceeds 1000-cap")
            print(f"  Would need to sub-split. Trying narrower buckets...")
            # Sub-split: try halving the range
            mid = (low + high) // 2
            for sub in [(low, mid), (mid, high)]:
                sub_n = bucket_count(sub[0], sub[1])
                print(f"    sub-bucket [{sub[0]}..{sub[1]}): {sub_n}")
                time.sleep(1.0)
        else:
            print(f"  largest bucket [{low}..{high}) has {n} records — fits under 1000-cap ✓")
            # Walk it
            collected = []
            page = 0
            while page * 100 < n:
                body = {
                    "query": {
                        "bool": {
                            "must": [{"term": {"artwork_type_id": 1}}],
                            "filter": [{"range": {"id": {"gte": low, "lt": high}}}],
                        }
                    },
                    "sort": [{"id": "asc"}],
                    "from": page * 100,
                    "size": 100,
                    "fields": ["id"],
                }
                status, data, err = post(ART_BASE + "/artworks/search", body)
                if status != 200:
                    print(f"    page {page} failed: status={status} err={err[:120]}")
                    break
                page_ids = [it["id"] for it in data["data"]]
                collected.extend(page_ids)
                print(f"    page {page}: got {len(page_ids)} ids (total collected: {len(collected)})")
                page += 1
                time.sleep(0.8)
            print(f"  collected {len(collected)} / {n} from bucket")

    section("Step 3 — Verify deep offset within a bucket (bucket has ~800 records)")
    # Use whatever bucket we found that's under 1000
    target_bucket = None
    for c in counts:
        if 500 <= c[2] < 1000:
            target_bucket = c
            break
    if target_bucket:
        low, high, n = target_bucket
        # Read offset (n - 100) (the last page)
        body = {
            "query": {
                "bool": {
                    "must": [{"term": {"artwork_type_id": 1}}],
                    "filter": [{"range": {"id": {"gte": low, "lt": high}}}],
                }
            },
            "sort": [{"id": "asc"}],
            "from": max(n - 100, 0),
            "size": 100,
            "fields": ["id"],
        }
        status, data, err = post(ART_BASE + "/artworks/search", body)
        if status == 200:
            ids = [it["id"] for it in data["data"]]
            print(f"  bucket [{low}..{high}) (n={n}) at offset {max(n-100,0)}: got {len(ids)} ids; last 3: {ids[-3:]}")
        else:
            print(f"  FAILED: status={status} err={err[:200]}")
    else:
        print("  no suitable bucket found")
    time.sleep(1.0)

    section("Step 4 — Test whether bool+range respects the 1000-cap PER BUCKET")
    # Create a query that DOES exceed 1000 within a bucket and confirm the cap fires
    # We'll use a wide range so the bucket has > 1000 records
    body = {
        "query": {
            "bool": {
                "must": [{"term": {"artwork_type_id": 1}}],
                "filter": [{"range": {"id": {"gte": 0, "lt": 300000}}}],
            }
        },
        "sort": [{"id": "asc"}],
        "from": 1000,
        "size": 1,
        "fields": ["id"],
    }
    status, data, err = post(ART_BASE + "/artworks/search", body)
    print(f"  from=1000 within wide bool: status={status}")
    if data:
        print(f"    got {len(data.get('data', []))} records, total={data.get('pagination',{}).get('total')}")
    else:
        print(f"    err: {err[:200]}")
    time.sleep(1.0)

    section("Step 5 — OAI-PMH endpoints (revisit, previous probe died on DNS)")
    oai_candidates = [
        "https://www.artic.edu/oai?verb=Identify",
        "https://www.artic.edu/api/oai?verb=Identify",
        "https://www.artic.edu/api/v1/oai?verb=Identify",
        "https://collections.artic.edu/oai?verb=Identify",
        "https://collection.artic.edu/oai?verb=Identify",
        "https://api.artic.edu/oai?verb=Identify",
        "https://api.artic.edu/v1/oai?verb=Identify",
        "https://api.artic.edu/api/v1/oai?verb=Identify",
    ]
    for url in oai_candidates:
        status, body, ctype = get(url, accept="application/xml")
        snippet = body[:160].replace("\n", " ").strip() if body else ""
        print(f"  [{status}] {url}: {snippet}")
        time.sleep(0.6)

    section("Step 6 — Check if an Authorization header is recognized")
    # The auth gate accepts Auth::check() = true via Laravel auth.
    # Most likely a Bearer token in Authorization header.
    # We don't have one but check what the response is.
    import urllib.request as ur
    req = ur.Request(ART_BASE + "/artworks/search?fields=id&from=1500&size=1")
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", "application/json")
    req.add_header("Authorization", "Bearer test-token-does-not-exist")
    try:
        with ur.urlopen(req, timeout=10) as r:
            body = r.read().decode()
            print(f"  [{r.status}] with bogus Bearer: {body[:240]}")
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        print(f"  [{e.code}] with bogus Bearer: {body[:240]}")
    time.sleep(1.0)

    section("Step 7 — Is there a public API key endpoint?")
    # Some museums (e.g. Rijks) used to require API keys obtainable freely.
    # Check known patterns at AIC.
    candidates = [
        "https://api.artic.edu/api/v1/register",
        "https://api.artic.edu/api/v1/auth",
        "https://api.artic.edu/api/v1/login",
        "https://www.artic.edu/api/access",
        "https://api.artic.edu/api/v1/me",
    ]
    for url in candidates:
        status, body, ctype = get(url)
        snippet = body[:160].replace("\n", " ").strip() if body else ""
        print(f"  [{status}] {url}: {snippet}")
        time.sleep(0.6)


if __name__ == "__main__":
    main()
