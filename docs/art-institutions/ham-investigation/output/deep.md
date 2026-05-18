# HAM — deep pagination probe

*Run at 2026-05-18 10:29:23 UTC*

## 1. Page-depth walk — classification=17 (Photographs)

From the facets probe: classification=17 has ~77 076 image-bearing records. p3a's worst case is `channel_offset + ai_cache_size = 8192` (when both are at their respective ceilings), so page ~82 is the binding constraint. We test well past that to find the actual cap.

| page | size | status | returned | first id | last id | total | pages |
|---|---|---|---|---|---|---|---|
| 1 | 100 | 200 | 100 | 1412 | 5043 | 77076 | 771 |
| 10 | 100 | 200 | 100 | 7914 | 8119 | 77076 | 771 |
| 50 | 100 | 200 | 100 | 19172 | 19387 | 77076 | 771 |
| 100 | 100 | 200 | 100 | 36052 | 36151 | 77076 | 771 |
| 200 | 100 | 200 | 100 | 51842 | 52090 | 77076 | 771 |
| 500 | 100 | 200 | 100 | 158940 | 159063 | 77076 | 771 |
| 770 | 100 | 200 | 100 | 381050 | 386143 | 77076 | 771 |
| 50 (repeat) | 100 | 200 | 100 | 19172 | 19387 | 77076 | 771 |

- In-run stability of page 50: first id on first call = 19172, on repeat = 19172

## 2. Beyond-last-page behavior

| page | size | status | returned | first id | total | pages |
|---|---|---|---|---|---|---|
| 1000 | 100 | 200 | 0 | None | 77076 | 771 |
| 5000 | 100 | 200 | 0 | None | 77076 | 771 |
| 10000 | 100 | 200 | 0 | None | None | None |

## 3. Sort comparison (default vs sort=id asc)

Critical for orphan eviction: if the default sort changes between refreshes (e.g. relevance scoring with date components), the same page=N returns different artworks and the orphan-eviction pass would churn the cache every cycle.

- **Default sort**, page 5 size 10 — first 10 ids: `[4669, 4670, 4671, 4672, 4673, 4674, 4675, 4767, 4768, 4769]`
- **sort=id asc**, page 5 size 10 — first 10 ids: `[4669, 4670, 4671, 4672, 4673, 4674, 4675, 4767, 4768, 4769]`

## 4. Page-size variation at moderate depth (page 50)

Confirms size=100 is the working maximum (matches basic probe), and that smaller sizes still produce the expected slice.

| size | status | returned | first id |
|---|---|---|---|
| 1 | 200 | 1 | 4769 |
| 25 | 200 | 25 | 8390 |
| 50 | 200 | 50 | 9621 |
| 100 | 200 | 100 | 19172 |

## 5. Translation to p3a's `(channel_offset, ai_cache_size)`

p3a stores `channel_offset` as an entry count, not a page number. The adapter computes `start_page = (channel_offset // size) + 1` and `within_page_skip = channel_offset % size`. Worst case (defaults): `channel_offset = 0`, `ai_cache_size = 1024` → 11 pages. Worst case (ceilings): `channel_offset = 4096`, `ai_cache_size = 4096` → page 41 through page 82.

Both worst cases land well under the deepest page that returned 200 above. No bool+range partition is needed for HAM.

