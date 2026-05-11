"""Pre-bake Rijksmuseum's curated Sets list.

The OAI-PMH ListSets endpoint at https://data.rijksmuseum.nl/oai does NOT
return CORS headers, so a browser cannot fetch it directly. This script
runs once (or whenever the set list is believed to have changed), pulls
the XML, and writes a static JSON file the static frontend can read.

Output: ../data/rijks-sets.json (relative to this script).

Run:  python build_rijks_sets.py
"""
from __future__ import annotations

import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import requests

OAI_URL = "https://data.rijksmuseum.nl/oai?verb=ListSets"
USER_AGENT = "museum-art-ubi-test/0.1 (+pub@kury.dev)"
OUT_PATH = Path(__file__).resolve().parent.parent / "data" / "rijks-sets.json"


def main() -> int:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    r = requests.get(OAI_URL, headers={"User-Agent": USER_AGENT}, timeout=30)
    r.raise_for_status()
    ns = {"oai": "http://www.openarchives.org/OAI/2.0/"}
    root = ET.fromstring(r.content)
    sets: list[dict[str, str]] = []
    for s in root.findall(".//oai:set", ns):
        spec = (s.findtext("oai:setSpec", namespaces=ns) or "").strip()
        name = (s.findtext("oai:setName", namespaces=ns) or "").strip()
        if spec and name:
            sets.append({"spec": spec, "name": name})
    OUT_PATH.write_text(
        json.dumps(sets, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"Wrote {len(sets)} sets to {OUT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
