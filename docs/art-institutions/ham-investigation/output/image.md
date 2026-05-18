# HAM — IIIF rendition + image-id length probe

*Run at 2026-05-18 10:31:22 UTC*

## 1. Image-id length distribution (C2)

Samples 25 records each across classification=17 (Photographs), classification=23 (Prints), classification=21 (Drawings), century=37525815 (20th century), culture=37526778 (American), place=2028213 (United States) — six populous terms — to characterize the shape and length of `baseimageurl` (which is what we'd store as iiif_key after stripping the host prefix).

- Total records collected: **150**
- Records with a `baseimageurl` populated: **54**
- Records missing `primaryimageurl`: **96**

**Length distribution of the URN portion (what we'd put in iiif_key):**

- min: 17 chars
- max: 26 chars
- median: 26 chars
- over 47 chars (won't fit in iiif_key[48]): 0/54

**Sample URNs (first 10):**

```
urn-3:HUAM:INV190170_dynmc  (26 chars)
urn-3:HUAM:INV190160_dynmc  (26 chars)
urn-3:HUAM:INV190162_dynmc  (26 chars)
urn-3:HUAM:INV190167_dynmc  (26 chars)
urn-3:HUAM:INV190169_dynmc  (26 chars)
urn-3:HUAM:INV190172_dynmc  (26 chars)
urn-3:HUAM:INV190154_dynmc  (26 chars)
urn-3:HUAM:INV190152_dynmc  (26 chars)
urn-3:HUAM:INV190166_dynmc  (26 chars)
urn-3:HUAM:INV208103_dynmc  (26 chars)
```

## 2. NRS → IDS redirect chain (C5)

Append `/full/!720,720/0/default.jpg` to the URN and follow redirects manually with `allow_redirects=False`. Document the chain so the firmware knows how many round trips it costs per image.

### Sample 1: object_id=1424, baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:INV190170_dynmc`

```
hop 0: 303 https://nrs.harvard.edu/urn-3:HUAM:INV190170_dynmc/full/!720,720/0/default.jpg
hop 1: 200 https://ids.lib.harvard.edu/mps/20493753/full/!720,720/0/default.jpg
```

### Sample 2: object_id=1425, baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:INV190160_dynmc`

```
hop 0: 303 https://nrs.harvard.edu/urn-3:HUAM:INV190160_dynmc/full/!720,720/0/default.jpg
hop 1: 200 https://ids.lib.harvard.edu/mps/20493743/full/!720,720/0/default.jpg
```

### Sample 3: object_id=4606, baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:INV190162_dynmc`

```
hop 0: 303 https://nrs.harvard.edu/urn-3:HUAM:INV190162_dynmc/full/!720,720/0/default.jpg
hop 1: 200 https://ids.lib.harvard.edu/mps/20493745/full/!720,720/0/default.jpg
```

## 3. Image rendition fetch — `!720,720` JPEG (C1, C3)

| # | object_id | status | content-type | size (bytes) | image dims |
|---|---|---|---|---|---|
| 1 | 1424 | 200 | `image/jpeg` | 75212 | 720x554 |
| 2 | 1425 | 200 | `image/jpeg` | 76176 | 720x558 |
| 3 | 4606 | 200 | `image/jpeg` | 73580 | 720x549 |
| 4 | 4607 | 200 | `image/jpeg` | 75174 | 720x555 |
| 5 | 4608 | 200 | `image/jpeg` | 76492 | 720x554 |
| 6 | 4620 | 200 | `image/jpeg` | 72169 | 720x552 |
| 7 | 4621 | 200 | `image/jpeg` | 73225 | 720x541 |
| 8 | 4622 | 200 | `image/jpeg` | 77309 | 720x554 |

**Constraint check:** p3a's `P3A_MAX_ARTWORK_SIZE = 16 MiB`. Any row above 16 777 216 bytes would need a smaller IIIF size cap.

## 4. Storage strategy for `iiif_key`

Two options for what to put in the 48-byte `iiif_key` slot:

**(a) Store the NRS URN** (e.g. `urn-3:HUAM:79762_dynmc`).
    - C-side `build_iiif_url`: `https://nrs.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`
    - Download path: one 303 redirect to IDS, then 200 JPEG. The Rijks
      adapter already implements manual redirect handling (HTTP_EVENT_ON_HEADER
      captures Location); we reuse the pattern.
    - No `resolve_entry` dispatch needed; this stays in the AIC/V&A bucket.

**(b) Pre-resolve to IDS at refresh time** (e.g. `mps/18483191`).
    - Refresh issues one HEAD per record to capture the 303 Location.
    - C-side `build_iiif_url`: `https://ids.lib.harvard.edu/{iiif_key}/full/!720,720/0/default.jpg`
    - Costs one extra HTTP per record during refresh; saves one round
      trip per image download.

Section 2 measured the actual redirect chain length. If it's a single
303 hop, option **(a)** is the cleaner choice — fewer moving parts,
no resolve_entry plumbing, and the refresh-time saving in option (b)
is wasted bandwidth because most refreshed entries never get downloaded
(the FIFO cache holds 1024+ entries but the player only consumes a few
per minute). REPORT.md cements this choice once the section 2 data is in.
