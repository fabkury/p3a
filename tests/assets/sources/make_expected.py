# SPDX-License-Identifier: Apache-2.0
"""What the sources' parsers should make of the recorded responses in this
folder, worked out here in Python: an implementation independent of the C
one the host tests check against it.

    python tests/assets/sources/make_expected.py

writes one `.expected` file per recorded listing. Run it when a response is
recorded again; the results are committed.

The responses were recorded from the live services with the requests the
device makes. They hold no API key. Giphy's are cut down to the fields the
device reads: the service adds analytics tokens and a client id inside each
media address, which belong to the account that recorded them.
"""
import calendar
import json
import re
from pathlib import Path

HERE = Path(__file__).parent
SCREEN = 720
ID = re.compile(r"^[0-9A-Za-z_-]{1,31}$")
DATE = re.compile(r"^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)$")
FILES = {"200.gif": 1, "giphy.gif": 5, "giphy-downsized-medium.gif": 7}


def when(text) -> int:
    m = DATE.match(text or "")
    if not m:
        return 0
    y, mo, d, h, mi, s = map(int, m.groups())
    if y < 1970 or not 1 <= mo <= 12 or not 1 <= d <= 31:
        return 0
    return calendar.timegm((y, mo, d, h, mi, s))


def size(v) -> int:
    try:
        n = int(v)
    except (TypeError, ValueError):
        return 0
    return n if 0 < n <= 65535 else 0


def giphy(name: str, prefer_downsized: bool, now: int) -> None:
    data = json.loads((HERE / f"{name}.json").read_bytes())["data"]
    lines = []
    for gif in data:
        if not isinstance(gif.get("id"), str) or not ID.match(gif["id"]):
            continue
        images = gif.get("images", {})
        fixed = images.get("fixed_height", {})
        file, w, h = 1, size(fixed.get("width")), size(fixed.get("height"))
        dm = images.get("downsized_medium", {})
        last = str(dm.get("url", "")).split("?")[0].split("#")[0].rsplit("/", 1)[-1]
        dw, dh = size(dm.get("width")), size(dm.get("height"))
        if prefer_downsized and last in ("giphy.gif", "giphy-downsized-medium.gif") and dw and dh \
                and dw * 2 <= SCREEN * 3 and dh * 2 <= SCREEN * 3:
            file, w, h = FILES[last], dw, dh
        at = when(gif.get("trending_datetime")) or when(gif.get("import_datetime")) or now
        lines.append(f"{gif['id']} {file} {w} {h} {at}")
    suffix = "downsized" if prefer_downsized else "fixed"
    (HERE / f"{name}.{suffix}.expected").write_text(f"{len(data)}\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, suffix, len(data), "items,", len(lines), "records,",
          sum(1 for line in lines if line.split()[1] != "1"), "downsized")


def klipy_pick(file: dict, webp: bool):
    """(url, width, height) of the file p3a takes, or None."""
    for fmt in (("webp", "gif") if webp else ("gif", "webp")):
        fit = best = None
        for tier in ("hd", "md", "sm", "xs"):
            one = (file.get(tier) or {}).get(fmt)
            if not isinstance(one, dict) or not str(one.get("url", "")).startswith("https://static.klipy.com/"):
                continue
            c = (one["url"], size(one.get("width")), size(one.get("height")))
            if best is None or c[1] * c[2] > best[1] * best[2]:
                best = c
            if c[1] * 2 <= SCREEN * 3 and c[2] * 2 <= SCREEN * 3 and (fit is None or c[1] * c[2] > fit[1] * fit[2]):
                fit = c
        if fit or best:
            return fit or best
    return None


def klipy(name: str, webp: bool) -> None:
    page = json.loads((HERE / f"{name}.json").read_bytes())["data"]
    lines = []
    for item in page["data"]:
        chosen = klipy_pick(item.get("file", {}), webp)
        if item.get("type") == "ad" or not isinstance(item.get("id"), int) or chosen is None:
            continue
        lines.append(f"{item['id']} {chosen[1]} {chosen[2]} {chosen[0]}")
    suffix = "webp" if webp else "gif"
    head = f"{len(page['data'])} {1 if page.get('has_next') else 0}"
    (HERE / f"{name}.{suffix}.expected").write_text(head + "\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, suffix, len(page["data"]), "items,", len(lines), "records")


# --- Makapix -------------------------------------------------------------------------

UUID = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
STAMP = re.compile(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(\.\d+)?(Z|\+00:00)$")
MAKAPIX_FORMATS = {"webp": 1, "gif": 2, "png": 3, "bmp": 5}


def stamp(text) -> int:
    m = STAMP.match(text or "")
    if not m:
        return 0
    return calendar.timegm(tuple(map(int, m.groups()[:6])))


def makapix(name: str, promoted: bool) -> None:
    """A page of either kind: the public feed ("items") or the player's own question ("posts").

    makapix_rpc_written.json was written by hand from the server's schema, because the player's
    question needs a registered player's token; replace it with a recorded answer when there is one.
    """
    import hashlib
    page = json.loads((HERE / f"{name}.json").read_bytes())
    posts = page["posts"] if "posts" in page else page["items"]
    lines = []
    for post in posts:
        number = post.get("post_id", post.get("id"))
        key = post.get("storage_key") or ""
        ending = (post.get("art_url") or "").split("?")[0].rsplit("/", 1)[-1].rpartition(".")
        fmt = MAKAPIX_FORMATS.get(ending[2] if ending[1] else "") or MAKAPIX_FORMATS.get(post.get("native_format"))
        if post.get("kind", "artwork") != "artwork" or not isinstance(number, int) or number < 1 \
                or not UUID.match(key) or not fmt:
            continue
        key = key.lower()
        at = (stamp(post.get("promoted_at")) if promoted else 0) or stamp(post.get("listed_at")) \
            or stamp(post.get("created_at"))
        digest = hashlib.sha256(key.encode()).digest()
        ext = {v: k for k, v in MAKAPIX_FORMATS.items()}[fmt]
        url = f"https://vault.makapix.club/{digest[0] & 0x3F:02x}/{digest[1] & 0x3F:02x}/{key}.{ext}"
        # Where the service itself says the file is (the device works it out from the key).
        said = post.get("art_url") or ""
        if said.endswith("." + ext) and name != "makapix_rpc_written":
            assert said == url, (said, url)
        lines.append(f"{number} {key} {fmt} {at} {stamp(post.get('artwork_modified_at'))} "
                     f"{post.get('width') or 0} {post.get('height') or 0} {digest.hex()} {url}")
    cursor = page.get("next_cursor") or ""
    more = bool(cursor) and ("items" in page or page.get("has_more") is True)
    suffix = "promoted" if promoted else "listed"
    head = f"{len(posts)} {1 if more else 0} {cursor or '-'}"
    (HERE / f"{name}.{suffix}.expected").write_text(head + "\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, suffix, len(posts), "posts,", len(lines), "records")


KEY = re.compile(r"^[0-9A-Za-z._~:-]{1,95}$")


def fitted(width, height) -> tuple:
    """An original's size as the 720-pixel picture p3a asks a museum for."""
    w, h = size_up_to(width, 100000), size_up_to(height, 100000)
    if not w or not h:
        return 0, 0
    longest = max(w, h)
    if longest > SCREEN:
        w, h = w * SCREEN // longest, h * SCREEN // longest
    return max(w, 1), max(h, 1)


def size_up_to(v, limit) -> int:
    return v if isinstance(v, int) and not isinstance(v, bool) and 0 < v <= limit else 0


def museum(name: str, items: int, more: bool, records: list) -> None:
    """records: (key, original width, original height)"""
    lines = []
    for key, w, h in records:
        if isinstance(key, str) and KEY.match(key) and key not in (".", ".."):
            fw, fh = fitted(w, h)
            lines.append(f"{key} {fw} {fh}")
    (HERE / f"{name}.expected").write_text(f"{items} {1 if more else 0}\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, items, "items,", len(lines), "records, more" if more else "records")


def vam(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    info = d.get("info", {})
    museum(name, len(d["records"]), bool(d["records"]) and info.get("page", 0) < info.get("pages", 0),
           [(r.get("_primaryImageId"), 0, 0) for r in d["records"]])


def smk(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    records = []
    for item in d["items"]:
        iiif = item.get("image_iiif_id")
        if isinstance(iiif, str) and "/iiif/jp2/" in iiif:
            records.append((iiif.rsplit("/iiif/jp2/", 1)[1], item.get("image_width"), item.get("image_height")))
    museum(name, len(d["items"]), bool(d["items"]) and d.get("offset", 0) + len(d["items"]) < d.get("found", 0), records)


def wellcome(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    records = []
    for work in d["results"]:
        for item in work.get("items", []):
            urls = [loc["url"] for loc in item.get("locations", [])
                    if loc.get("locationType", {}).get("id") == "iiif-image" and "/image/" in loc.get("url", "")]
            if urls:
                records.append((re.split(r"[/?#]", urls[0].split("/image/", 1)[1])[0], 0, 0))
                break
    museum(name, len(d["results"]), bool(d["results"]) and bool(d.get("nextPage")), records)


def cma(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())

    def text_size(v):
        return int(v) if isinstance(v, str) and v.isdigit() else 0

    records = []
    for art in d["data"]:
        web = (art.get("images") or {}).get("web") or {}
        if isinstance(art.get("accession_number"), str) and web.get("url"):
            records.append((art["accession_number"], text_size(web.get("width")), text_size(web.get("height"))))
    info = d.get("info", {})
    museum(name, len(d["data"]),
           bool(d["data"]) and info.get("parameters", {}).get("skip", 0) + len(d["data"]) < info.get("total", 0), records)


def mia(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    hits = [] if isinstance(d, list) else d["hits"]["hits"]
    records = [(str(h["_source"]["id"]), h["_source"].get("image_width"), h["_source"].get("image_height"))
               for h in hits if isinstance(h.get("_source", {}).get("id"), int)]
    museum(name, len(hits), len(hits) >= 50, records)


def ham(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    prefix = "https://nrs.harvard.edu/"
    records = [(r["primaryimageurl"][len(prefix):], 0, 0) for r in d["records"]
               if isinstance(r.get("primaryimageurl"), str) and r["primaryimageurl"].startswith(prefix)]
    info = d.get("info", {})
    museum(name, len(d["records"]), bool(d["records"]) and info.get("page", 0) < info.get("pages", 0), records)


def si(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    rows = d["response"]["rows"]

    def text_size(v):  # a number, or a number as text
        if isinstance(v, int) and not isinstance(v, bool):
            return v
        return int(v) if isinstance(v, str) and v.isdigit() else 0

    records = []
    for row in rows:
        media = row.get("content", {}).get("descriptiveNonRepeating", {}).get("online_media", {}).get("media")
        first = media[0] if isinstance(media, list) and media else media
        if isinstance(first, dict) and isinstance(first.get("idsId"), str):
            records.append((first["idsId"], text_size(first.get("width")), text_size(first.get("height"))))
    museum(name, len(rows), len(rows) >= 50, records)


def rijks(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    prefix = "https://id.rijksmuseum.nl/"
    records = [(i["id"][len(prefix):], 0, 0) for i in d["orderedItems"]
               if isinstance(i.get("id"), str) and i["id"].startswith(prefix) and i["id"][len(prefix):].isdigit()]
    nxt = (d.get("next") or {}).get("id", "")
    more = bool(d["orderedItems"]) and nxt.startswith("https://data.rijksmuseum.nl/search/collection?") and len(nxt) < 384
    museum(name, len(d["orderedItems"]), more, records)


def artic(name: str) -> None:
    d = json.loads((HERE / f"{name}.json").read_bytes())
    p = d.get("pagination", {})
    museum(name, len(d["data"]),
           bool(d["data"]) and p.get("current_page", 0) < p.get("total_pages", 0) and p.get("current_page", 0) < 10,
           [(a.get("image_id"), 0, 0) for a in d["data"]])


if __name__ == "__main__":
    for listing in ("rijks_set_p1", "rijks_set_p2", "rijks_empty"):
        rijks(listing)
    artic("artic_department_p1")
    for listing in ("ham_classification_p1", "ham_classification_p3", "ham_empty"):
        ham(listing)
    for listing in ("si_unit_p1", "si_empty"):
        si(listing)
    for listing in ("wellcome_worktype_p1", "wellcome_empty"):
        wellcome(listing)
    for listing in ("cma_department_p1", "cma_type_p2", "cma_empty"):
        cma(listing)
    for listing in ("mia_classification_p1", "mia_empty", "mia_past_window"):
        mia(listing)
    for listing in ("vam_category_p1", "vam_category_p3", "vam_empty"):
        vam(listing)
    for listing in ("smk_collection_p1", "smk_empty"):
        smk(listing)
    for listing in ("makapix_promoted_p1", "makapix_promoted_p2", "makapix_rpc_written"):
        makapix(listing, True)
        makapix(listing, False)
    for listing in ("klipy_gifs_trending", "klipy_stickers_search", "klipy_gifs_item"):
        klipy(listing, False)
        klipy(listing, True)
    for listing in ("giphy_trending", "giphy_search"):
        giphy(listing, True, 1700000000)
        giphy(listing, False, 1700000000)
