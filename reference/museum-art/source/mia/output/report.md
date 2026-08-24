# Mia API probe report

## 1. Corpus + live aggregations
- scope: `rights_type:"Public Domain" AND image:valid`
- total: **10000** (relation gte)
- Classification: 200 buckets, 161 usable (dropped: 39 over-32B, 0 quote/backslash, 0 malformed-unicode; 8 kept with '/'), top: [('Prints', 10426), ('Textiles', 3158), ('Ceramics', 2740)]
- Department: 6 buckets, 6 usable (dropped: 0 over-32B, 0 quote/backslash, 0 malformed-unicode; 0 kept with '/'), top: [('Asian Art', 14229), ('European Art', 10583), ('Arts of the Americas', 7995)]
- Country: 200 buckets, 197 usable (dropped: 3 over-32B, 0 quote/backslash, 0 malformed-unicode; 0 kept with '/'), top: [('United States', 7228), ('Japan', 6290), ('China', 5691)]
- Style: 189 buckets, 172 usable (dropped: 17 over-32B, 0 quote/backslash, 0 malformed-unicode; 0 kept with '/'), top: [('Art Nouveau', 78), ('Art Deco', 72), ('Kano', 66)]

## 2. Listing + pagination
- `classification:"Paintings" AND rights_type:"Public Domain" AND image:valid`: total {'value': 2555, 'relation': 'eq'}, page-1 ids [4418, 1978, 529, 1226, 537]
- from=9990: envelope OK, 1 hit(s)
- from=10000 (past ES window): HTTP 500 (alternate past-window failure shape)

## 3. Single-record lookup
- `id:4418` -> title 'Portrait of Lucia Wijbrants', artist 'Gabriël Metsu', dated '1667'

## 4. Image buckets
- /400/4418.jpg -> HTTP 200, 37959 bytes
- /800/4418.jpg -> HTTP 200, 119256 bytes
- /720/4418.jpg -> HTTP 403, 243 bytes
- /800/999999999.jpg (bogus id) -> HTTP 403
- downloaded 4418: 119256 bytes, JPEG magic ok: True
- downloaded 1978: 75735 bytes, JPEG magic ok: True

Run summary: all probes completed.
