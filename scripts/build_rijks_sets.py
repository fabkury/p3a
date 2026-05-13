"""Pre-bake Rijksmuseum's curated Sets list.

The OAI-PMH ListSets endpoint at https://data.rijksmuseum.nl/oai does NOT
return CORS headers, so a browser cannot fetch it directly. This script
runs once (or whenever the set list is believed to have changed), pulls
the XML, and writes a static JSON file the firmware serves from
LittleFS so the browser-side Rijks adapter can load it from the device.

Default output: webui/museum/rijks-sets.json (relative to repo root).

Run:  python scripts/build_rijks_sets.py
      python scripts/build_rijks_sets.py --out /tmp/rijks-sets.json
"""
from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import requests

OAI_URL = "https://data.rijksmuseum.nl/oai?verb=ListSets"
USER_AGENT = "p3a-rijks-sets-builder/1.0 (+pub@kury.dev)"
DEFAULT_OUT = (
    Path(__file__).resolve().parent.parent / "webui" / "museum" / "rijks-sets.json"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output path (default: {DEFAULT_OUT})",
    )
    args = parser.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
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
    args.out.write_text(
        json.dumps(sets, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"Wrote {len(sets)} sets to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
