"""One-shot: sum totalItems across all Rijksmuseum Sets (double-counting allowed).

For each Set returned by OAI-PMH ListSets, fetch the first page of
search/collection?memberOfSetId=… and read `partOf.totalItems`. Sum, and
compare against the unfiltered corpus total.
"""
from __future__ import annotations

import urllib.parse
import xml.etree.ElementTree as ET

import requests

BASE = "https://data.rijksmuseum.nl"
ID_PREFIX = "https://id.rijksmuseum.nl/"
S = requests.Session()
S.headers.update({"User-Agent": "museum-art-explore/0.1"})


def list_sets() -> list[tuple[str, str]]:
    r = S.get(f"{BASE}/oai?verb=ListSets", timeout=30)
    r.raise_for_status()
    ns = {"oai": "http://www.openarchives.org/OAI/2.0/"}
    root = ET.fromstring(r.content)
    out = []
    for s in root.findall(".//oai:set", ns):
        spec = (s.findtext("oai:setSpec", namespaces=ns) or "").strip()
        name = (s.findtext("oai:setName", namespaces=ns) or "").strip()
        if spec:
            out.append((spec, name))
    return out


def total_items(url: str) -> int | None:
    r = S.get(url, headers={"Accept": "application/ld+json"}, timeout=30)
    r.raise_for_status()
    d = r.json()
    p = d.get("partOf")
    if isinstance(p, dict) and "totalItems" in p:
        return p["totalItems"]
    return d.get("totalItems")


def main() -> None:
    sets = list_sets()
    print(f"Sets enumerated: {len(sets)}")

    corpus = total_items(f"{BASE}/search/collection")
    corpus_img = total_items(f"{BASE}/search/collection?imageAvailable=true")
    print(f"Corpus total (unfiltered):              {corpus:,}")
    print(f"Corpus total (imageAvailable=true):     {corpus_img:,}")
    print()

    rows: list[tuple[str, str, int | None]] = []
    for i, (spec, name) in enumerate(sets, 1):
        member_id = f"{ID_PREFIX}{spec}"
        url = f"{BASE}/search/collection?memberOfSetId={urllib.parse.quote(member_id, safe='')}"
        try:
            t = total_items(url)
        except Exception as e:
            print(f"  [{i:>3}/{len(sets)}] {spec} ERROR: {e}")
            rows.append((spec, name, None))
            continue
        rows.append((spec, name, t))
        if i % 25 == 0 or i == len(sets):
            print(f"  [{i:>3}/{len(sets)}] last: {spec} = {t}")

    counted = [r for r in rows if r[2] is not None]
    failed = [r for r in rows if r[2] is None]
    total = sum(r[2] for r in counted)
    print()
    print("=" * 60)
    print(f"Sets queried:         {len(sets)}")
    print(f"Sets returning count: {len(counted)}")
    print(f"Sets erroring:        {len(failed)}")
    print(f"Sum of set totals (double-counts allowed): {total:,}")
    print(f"As fraction of corpus: {total / corpus:.2%}")
    print()

    rows_sorted = sorted(counted, key=lambda r: r[2], reverse=True)
    print("Top 10 largest sets:")
    print(f"  {'spec':>8} {'count':>10}  name")
    for spec, name, t in rows_sorted[:10]:
        print(f"  {spec:>8} {t:>10,}  {name[:65]}")
    print()
    print("Sets with 0 items:", sum(1 for _, _, t in counted if t == 0))
    print("Sets with <10 items:", sum(1 for _, _, t in counted if t < 10))


if __name__ == "__main__":
    main()
