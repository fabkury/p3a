# CMA API probe report

## 1. Corpus + vocabularies
- CC0-with-image total: **41507**
- limit=1000 honored: got 1000 records in one page
- distinct departments in sample: 18; types: 34
- names over 32 chars (identifier slot): ['Egyptian and Ancient Near Eastern Art', 'Modern European Painting and Sculpture']
- accession charset: max len 14, non-[A-Za-z0-9.-] count: 0

## 2. Pagination
- department=Drawings: total 2621, page-1 ids ['1926.1976', '1926.25', '2012.4', '1927.208', '1949.544']
- deep skip=40000 (no filter): returned 1 record(s) — no offset cap

## 3. Filter exact-match requirement
- department="Modern European Painting and Sculpture": 486
- department="Modern European Painting and Scu" (32-char truncation): 0
- => filters demand the exact full name; truncated playset ids must be expanded (CMA_TERM_EXPANSION in museums/cma.c)

## 4. Single-record lookup
- GET /api/artworks/1915.534 -> title 'Nathaniel Hurd', creators: 1

## 5. Web-rendition template (100-record sample at skip=500)
- records without images.web.url: 0
- records with empty width/filesize strings: 2
- template mismatches (https://openaccess-cdn.clevelandart.org/{acc}/{acc}_web.jpg): 0 []
- downloaded 1944.80: 675343 bytes, JPEG magic ok: True
- downloaded 1957.139: 651799 bytes, JPEG magic ok: True

Run summary: all probes completed.
