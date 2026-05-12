"""
Probe the exact 403 boundary on AIC's /artworks/search.

The first probe showed that page=11&limit=100 already 403s on
artwork_type_id=1 (Painting), much earlier than the design doc's
"past page ~10" suggested. We need to know:

  - Is the boundary an *offset* threshold or a *page-number* threshold?
    (Equivalently: does limit affect when 403 kicks in?)
  - Does the ES-style from=N parameter bypass the boundary?
  - Does the 403 carry a body / Retry-After / WWW-Authenticate?
  - Does the boundary depend on facet size (small vs. huge result set)?
  - Does sort= change behavior?

Tests use Painting (artwork_type_id=1, total ~3794) for the failing
case and PC-4 department (total ~2195) for the passing case.

Spaced 1.5s between requests to stay polite under AIC's 60/min cap.
"""

import json
import time
import urllib.parse
import urllib.request

BASE = "https://api.artic.edu/api/v1"
USER_AGENT = "p3a-offset-probe/1.0 (pub@kury.dev)"


def fetch_with_headers(url: str) -> tuple[int, dict | None, str, dict]:
    req = urllib.request.Request(url)
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            status = r.status
            headers = dict(r.getheaders())
    except urllib.error.HTTPError as e:
        body = e.read()
        status = e.code
        headers = dict(e.headers.items())
    raw_head = body[:400].decode("utf-8", errors="replace")
    try:
        data = json.loads(body.decode("utf-8"))
    except Exception:
        data = None
    return status, data, raw_head, headers


def build_url(filter_pair, **params) -> str:
    qkey, qval = filter_pair
    q = [(qkey, qval), ("fields", "id")]
    for k, v in params.items():
        q.append((k, str(v)))
    return f"{BASE}/artworks/search?" + urllib.parse.urlencode(q)


PAINTING = ("query[term][artwork_type_id]", "1")
PC4 = ("query[term][department_id]", "PC-4")


def probe(label: str, filter_pair, **params) -> int:
    url = build_url(filter_pair, **params)
    status, data, head, headers = fetch_with_headers(url)
    info = ""
    if status == 200 and data:
        pagination = data.get("pagination", {})
        n_ids = len(data.get("data", []))
        info = f"total={pagination.get('total')} cur_page={pagination.get('current_page')} offset={pagination.get('offset')} got={n_ids}"
    elif status >= 400:
        retry = headers.get("Retry-After") or headers.get("retry-after") or "-"
        body_snip = head.strip().replace("\n", " ")[:160]
        info = f"Retry-After={retry} body={body_snip!r}"
    print(f"  [{status}] {label}: {info}")
    return status


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def main() -> None:
    section("Map 403 boundary on Painting (artwork_type_id=1, total ~3794)")
    # Sweep page=N at limit=100. Goal: find the smallest page that 403s.
    boundaries_to_try = [1, 2, 3, 5, 8, 10, 11, 15, 20, 30, 38]
    for pg in boundaries_to_try:
        probe(f"page={pg} limit=100", PAINTING, page=pg, limit=100)
        time.sleep(1.5)

    section("Same offsets but via from=N&size=100 (ES path)")
    # If from= bypasses the boundary, this maps a usable offset window.
    for offset in [0, 100, 500, 1000, 1100, 1500, 3000, 5000, 9000, 9900, 10000, 12000]:
        probe(f"from={offset} size=100", PAINTING, **{"from": offset, "size": 100})
        time.sleep(1.5)

    section("Does limit=10 change the boundary? (offset 1000 = page 101 at limit 10)")
    for pg in [50, 100, 101, 150, 200, 300]:
        probe(f"page={pg} limit=10", PAINTING, page=pg, limit=10)
        time.sleep(1.5)

    section("PC-4 dept (total ~2195): page=11&limit=100 should succeed")
    for pg in [5, 10, 11, 15, 20, 22]:
        probe(f"page={pg} limit=100", PC4, page=pg, limit=100)
        time.sleep(1.5)

    section("PC-4 dept: from=N&size=100 should equally succeed")
    for offset in [0, 500, 1000, 1100, 1500, 2100, 2195]:
        probe(f"from={offset} size=100", PC4, **{"from": offset, "size": 100})
        time.sleep(1.5)

    section("Does sort= move the boundary on Painting?")
    for pg in [10, 11, 12]:
        probe(f"page={pg} limit=100 sort=id", PAINTING, page=pg, limit=100, sort="id")
        time.sleep(1.5)
    for offset in [1000, 1100, 2000]:
        probe(f"from={offset} size=100 sort=id", PAINTING, **{"from": offset, "size": 100, "sort": "id"})
        time.sleep(1.5)


if __name__ == "__main__":
    main()
