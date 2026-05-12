"""
Probe Rijksmuseum's offset capabilities on /search/collection.

The firmware uses cursor-walk pagination via Linked-Art
OrderedCollectionPage (HMO → VisualItem → DigitalObject). There is
no `?page=` parameter; instead the response contains `next` with a URL
to follow.

We need to know:
  - Does Rijks expose any direct offset / pageToken / from parameter?
  - What's the `next` URL shape — is the offset encoded in a way we
    could synthesize without walking?
  - How big are typical sets? (caps the realistic offset range)
  - Are there alternative endpoints (e.g. legacy api?key=...) that
    DO support offset?

Tests:
  1. First page on a known set (Rembrandt Self-Portraits, set 26118
     based on docs — adjust if not found).
  2. Walk N pages, log every `next` URL to map the pagination shape.
  3. Probe direct offset parameters: ?from=N, ?offset=N, ?page=N,
     ?pageNumber=N, ?startIndex=N on the same endpoint.
  4. Probe the legacy rijksmuseum.nl/api with `p=` parameter.

Reference:
  https://data.rijksmuseum.nl/docs/api/
  https://www.rijksmuseum.nl/api/getting-started/ (legacy)
"""

import json
import time
import urllib.parse
import urllib.request

DATA_BASE = "https://data.rijksmuseum.nl"
# Largest curated set: 'all paintings' or similar — discovered dynamically.
# We'll start by listing some sets via OAI-PMH or fall back to a known set ID.

# Set IDs from the actual rijks-sets.json (if available) or from the test
# we'll find a large one programmatically.
TEST_SETS = [
    "26118",     # Cursed but commonly cited
    "23",        # Frequently appears in examples
    "13",        # Numbered low — probably old curated set
]


def fetch(url: str, accept: str = "application/ld+json") -> tuple[int, dict | None, str, dict]:
    req = urllib.request.Request(url)
    req.add_header("Accept", accept)
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            body = r.read()
            status = r.status
            headers = dict(r.getheaders())
    except urllib.error.HTTPError as e:
        body = e.read()
        status = e.code
        headers = dict(e.headers.items())
    raw_head = body[:500].decode("utf-8", errors="replace")
    try:
        data = json.loads(body.decode("utf-8"))
    except Exception:
        data = None
    return status, data, raw_head, headers


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def build_listing(set_id: str, extra_params: dict | None = None) -> str:
    params = [
        ("memberOfSetId", f"https://id.rijksmuseum.nl/{set_id}"),
        ("imageAvailable", "true"),
    ]
    if extra_params:
        params.extend(extra_params.items())
    return f"{DATA_BASE}/search/collection?" + urllib.parse.urlencode(params)


def walk_pages(set_id: str, max_pages: int = 3) -> list[dict]:
    """Walk up to max_pages pages, recording the URL shape at each hop."""
    url = build_listing(set_id)
    hops = []
    for hop in range(max_pages):
        print(f"  hop {hop + 1}: {url}")
        status, data, head, headers = fetch(url)
        print(f"    status={status}")
        if status != 200 or not data:
            print(f"    body head: {head[:200]}")
            break
        n_items = len(data.get("orderedItems", []))
        total = data.get("partOf", {}).get("totalItems") or data.get("totalItems")
        next_url = data.get("next")
        if isinstance(next_url, dict):
            next_url = next_url.get("id") or next_url.get("@id")
        # In some Linked-Art docs partOf carries first/last/next
        first = data.get("first")
        last = data.get("last")
        if isinstance(first, dict):
            first = first.get("id") or first.get("@id")
        if isinstance(last, dict):
            last = last.get("id") or last.get("@id")
        print(f"    orderedItems: {n_items}, totalItems: {total}, has next: {bool(next_url)}")
        if first:
            print(f"    first: {first}")
        if last:
            print(f"    last: {last}")
        if next_url:
            print(f"    next: {next_url}")
        hops.append({
            "hop": hop,
            "url": url,
            "n_items": n_items,
            "total": total,
            "next": next_url,
            "first_id_seen": (
                (data["orderedItems"][0].get("id") if data.get("orderedItems") else None)
            ),
        })
        if not next_url:
            break
        url = next_url
        time.sleep(1.0)
    return hops


def find_a_large_set() -> str | None:
    """Try OAI-PMH ListSets fallback; otherwise return None."""
    # OAI-PMH has no CORS but server-to-server is fine.
    url = f"{DATA_BASE}/oai?verb=ListSets"
    try:
        with urllib.request.urlopen(url, timeout=15) as r:
            body = r.read().decode("utf-8", errors="replace")
        # Heuristic — pull the first 30 setSpec values
        import re
        specs = re.findall(r"<setSpec>([^<]+)</setSpec>", body)
        return specs[:5] if specs else None
    except Exception as e:
        print(f"  OAI-PMH fetch failed: {e}")
        return None


def main() -> None:
    section("Discover available sets via OAI-PMH ListSets")
    specs = find_a_large_set()
    if specs:
        print(f"  first 5 setSpec values: {specs}")

    # Pick first candidate that returns >= 100 items
    test_set = None
    for candidate in (specs or []) + TEST_SETS:
        url = build_listing(candidate)
        status, data, head, headers = fetch(url)
        if status == 200 and data:
            total = data.get("partOf", {}).get("totalItems") or data.get("totalItems") or len(data.get("orderedItems", []))
            print(f"  candidate {candidate!r}: status=200 total~{total}")
            if (total or 0) >= 100:
                test_set = candidate
                break
        else:
            print(f"  candidate {candidate!r}: status={status} head={head[:120]}")
        time.sleep(0.8)

    if not test_set:
        print("  No suitable set found; falling back to '26118'")
        test_set = "26118"
    print(f"\n  Using set: {test_set}")

    section(f"Walk first 3 pages of set {test_set}; log URL shapes")
    hops = walk_pages(test_set, max_pages=3)

    section("Try direct offset-style params on the search/collection endpoint")
    candidates = [
        ("from", "100"),
        ("offset", "100"),
        ("page", "2"),
        ("pageNumber", "2"),
        ("startIndex", "100"),
        ("start", "100"),
        ("ps", "10"),  # page size
        ("pageSize", "10"),
        ("p", "2"),
        ("skip", "10"),
        ("cursor", "100"),
        ("pageToken", "100"),
    ]
    for k, v in candidates:
        url = build_listing(test_set, {k: v})
        status, data, head, headers = fetch(url)
        first_id = None
        n_items = 0
        if status == 200 and data:
            items = data.get("orderedItems") or []
            n_items = len(items)
            if items:
                first_id = items[0].get("id")
        print(f"  [{status}] {k}={v}: n_items={n_items} first_id={first_id}")
        time.sleep(0.8)

    section("Compare next URL shape between hop 0 and hop 1")
    if len(hops) >= 2:
        print(f"  hop0 next: {hops[0]['next']}")
        print(f"  hop1 next: {hops[1]['next']}")
        try:
            u0 = urllib.parse.urlparse(hops[0]["next"]) if hops[0]["next"] else None
            u1 = urllib.parse.urlparse(hops[1]["next"]) if hops[1]["next"] else None
            if u0:
                print(f"    hop0 next query: {urllib.parse.parse_qs(u0.query)}")
            if u1:
                print(f"    hop1 next query: {urllib.parse.parse_qs(u1.query)}")
        except Exception as e:
            print(f"  URL parse error: {e}")

    section("Probe the legacy rijksmuseum.nl/api/en/collection endpoint")
    # The legacy API requires an API key; we test the public unauthenticated
    # endpoint just to see what it returns. The shape might support `p=`.
    url = "https://www.rijksmuseum.nl/api/en/collection?key=0fiuZFh4&ps=2&p=1"
    status, data, head, headers = fetch(url, accept="application/json")
    print(f"  [{status}] legacy api p=1 ps=2: head={head[:240]}")
    time.sleep(0.5)
    url = "https://www.rijksmuseum.nl/api/en/collection?key=0fiuZFh4&ps=10&p=20"
    status, data, head, headers = fetch(url, accept="application/json")
    print(f"  [{status}] legacy api p=20 ps=10: head={head[:240]}")


if __name__ == "__main__":
    main()
