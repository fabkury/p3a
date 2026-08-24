"""Pre-bake Cleveland Museum of Art's browse-axis term lists.

CMA's Open Access API has no facet-enumeration endpoint (no
/api/departments/), so the department and type vocabularies cannot be
discovered by the browser at runtime. This script scans the full
CC0-with-image corpus (~42 requests at limit=1000), tallies the distinct
`department` and `type` values with their counts, and writes a static
JSON file the firmware serves from LittleFS so the browser-side CMA
adapter can load it from the device.

Names longer than 32 chars (the playset identifier slot) get a 32-char
truncated `id` plus a `query` field carrying the exact full name the API
requires. The device-side mirror of that expansion lives in
components/art_institution/museums/cma.c (CMA_TERM_EXPANSION) — this
script prints the table it baked so the C mirror can be kept in sync.

Run once, or whenever the vocabularies are believed to have changed
(typically at release time):

  python scripts/build_cma_terms.py
  python scripts/build_cma_terms.py --out /tmp/cma-terms.json
"""
from __future__ import annotations

import argparse
import datetime
import json
import sys
from collections import Counter
from pathlib import Path

import requests

API_URL = "https://openaccess-api.clevelandart.org/api/artworks/"
USER_AGENT = "p3a-cma-terms-builder/1.0 (+pub@kury.dev)"
PAGE_LIMIT = 1000
MAX_ID_CHARS = 32  # playset identifier[33] slot
DEFAULT_OUT = (
    Path(__file__).resolve().parent.parent / "webui" / "museum" / "cma-terms.json"
)


def scan_corpus() -> tuple[int, Counter, Counter]:
    """One pass over every CC0-with-image record, tallying both axes."""
    departments: Counter = Counter()
    types: Counter = Counter()
    total = 0
    skip = 0
    session = requests.Session()
    session.headers["User-Agent"] = USER_AGENT
    while True:
        r = session.get(
            API_URL,
            params={
                "cc0": "1",
                "has_image": "1",
                "skip": skip,
                "limit": PAGE_LIMIT,
                "fields": "department,type",
            },
            timeout=60,
        )
        r.raise_for_status()
        body = r.json()
        total = int(body["info"]["total"])
        data = body.get("data", [])
        if not data:
            break
        for rec in data:
            dep = rec.get("department")
            typ = rec.get("type")
            if isinstance(dep, str) and dep.strip():
                departments[dep.strip()] += 1
            if isinstance(typ, str) and typ.strip():
                types[typ.strip()] += 1
        skip += len(data)
        print(f"  scanned {skip}/{total}", file=sys.stderr)
        if skip >= total:
            break
    return total, departments, types


def build_axis(counter: Counter) -> tuple[list[dict], list[tuple[str, str]]]:
    """Counter -> sorted term list; returns (terms, expansion_pairs)."""
    terms: list[dict] = []
    expansions: list[tuple[str, str]] = []
    ids_seen: set[str] = set()
    for name, count in sorted(counter.items(), key=lambda kv: (-kv[1], kv[0])):
        if len(name) > MAX_ID_CHARS:
            term_id = name[:MAX_ID_CHARS]
            expansions.append((term_id, name))
        else:
            term_id = name
        if term_id in ids_seen:
            # Two full names sharing a 32-char prefix would make the
            # truncated identifier ambiguous on the device side.
            raise SystemExit(f"FATAL: truncation collision on id {term_id!r}")
        ids_seen.add(term_id)
        term = {"id": term_id, "label": name, "count": count}
        if term_id != name:
            term["query"] = name
        terms.append(term)
    return terms, expansions


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output path (default: {DEFAULT_OUT})",
    )
    args = parser.parse_args()

    print("Scanning CMA corpus...", file=sys.stderr)
    total, departments, types = scan_corpus()
    dep_terms, dep_exp = build_axis(departments)
    type_terms, type_exp = build_axis(types)

    out = {
        "generated": datetime.datetime.now(datetime.timezone.utc)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "api": API_URL,
        "filters": "cc0=1&has_image=1",
        "total": total,
        "axes": {
            "department": dep_terms,
            "type": type_terms,
        },
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"Wrote {len(dep_terms)} departments + {len(type_terms)} types "
        f"({total} works) to {args.out}"
    )

    expansions = dep_exp + type_exp
    if expansions:
        print(
            "\nTruncated ids baked (keep CMA_TERM_EXPANSION in "
            "components/art_institution/museums/cma.c in sync):"
        )
        for term_id, full in expansions:
            print(f'    {{ "{term_id}", "{full}" }},')
    else:
        print("\nNo truncated ids — CMA_TERM_EXPANSION entries are unused.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
