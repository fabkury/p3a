# HAM — basic surface probe

*Run at 2026-05-18 10:24:45 UTC*

## 1. Authentication model (D1/D2)

- **GET /object without apikey**: HTTP 401
  - Response excerpt: `Unauthorized…`
- **GET /object with invalid apikey**: HTTP 401
  - Response excerpt: `Unauthorized…`
- **GET /object with valid apikey**: HTTP 200
  - Headers (interesting subset):
    - `Content-Type: application/json; charset=utf-8`
    - `Access-Control-Allow-Origin: *`

## 2. Listing envelope and metadata (A3, A5, B3, B6)

### Envelope `info` block

```json
{
  "totalrecordsperquery": 5,
  "totalrecords": 5575,
  "pages": 1115,
  "page": 1,
  "next": "https://api.harvardartmuseums.org/object?apikey=c09e5b21-5ea4-4762-b611-e41d3a2ba07d&size=5&page=2&hasimage=1&classification=26&fields=id%2Cobjectnumber%2Ctitle%2Cdated%2Cpeople%2Cclassification%2Cprimaryimageurl%2Cimages%2Curl",
  "responsetime": "27 ms"
}
```

- **records count returned**: 5
### Per-record fields (first record)

```json
{
  "id": 1429,
  "objectnumber": "2006.13",
  "dated": "2005",
  "classification": "Paintings",
  "imagepermissionlevel": 0,
  "url": "https://www.harvardartmuseums.org/collections/object/1429",
  "images": [
    {
      "date": null,
      "copyright": null,
      "imageid": 207486,
      "format": "image/jpeg",
      "description": null,
      "technique": null,
      "renditionnumber": "79762",
      "displayorder": 1,
      "baseimageurl": "https://nrs.harvard.edu/urn-3:HUAM:79762_dynmc",
      "alttext": null,
      "width": 1024,
      "publiccaption": "Painting, section 1",
      "height": 298
    },
    {
      "date": null,
      "copyright": null,
      "imageid": 207485,
      "format": "image/jpeg",
      "description": null,
      "technique": null,
      "renditionnumber": "79761",
      "displayorder": 2,
      "baseimageurl": "https://nrs.harvard.edu/urn-3:HUAM:79761_dynmc",
      "alttext": null,
      "width": 1024,
      "publiccaption": "Title",
      "height": 325
    },
    {
      "date": null,
      "copyright": null,
      "imageid": 207487,
      "format": "image/jpeg",
      "description": null,
      "technique": null,
      "renditionnumber": "79763",
      "displayorder": 3,
      "baseimageurl": "https://nrs.harvard.edu/urn-3:HUAM:79763_dynmc",
      "alttext": null,
      "width": 1024,
      "publiccaption": "Painting, section 2",
      "height": 299
    },
    {
      "date": null,
      "copyright": null,
      "imageid": 207488,
      "format": "image/jpeg",
      "description": null,
      "technique": null,
      "renditionnumber": "79764",
      "displayorder": 4,
      "baseimageurl": "https://nrs.harvard.edu/urn-3:HUAM:79764_dynmc",
      "alttext": null,
      "width": 1024,
      "publiccaption": "Painting, section 3",
      "height": 300
    },
    {
      "date": null,
      "copyright": null,
      "imageid": 207489,
      "format": "image/jpeg",
      "description": null,
      "technique": null,
      "renditionnumber
```

**Field availability sanity check on the 5 returned records:**

  - `title`: present on 5/5 records
  - `dated`: present on 5/5 records
  - `primaryimageurl`: present on 4/5 records
  - `people`: present on 5/5 records
  - `classification`: present on 5/5 records
  - `id`: present on 5/5 records

## 3. hasimage filter (A4)

- classification=26 hasimage=0: totalrecords = `324`
- classification=26 hasimage=1: totalrecords = `5575`
- classification=26 (no hasimage param): totalrecords = `5899`

## 4. Sort options (B5 prep)

- sort=id_asc: first 3 object ids = [1429, 4586, 4615]
- sort=dated_asc: first 3 object ids = []
- sort=rank_default: first 3 object ids = [1429, 4586, 4615]

## 5. Page-size ceiling (B2 sanity)

- size=10: HTTP 200, returned 10 records, `info.totalrecordsperquery`=10
- size=100: HTTP 200, returned 100 records, `info.totalrecordsperquery`=100
- size=200: HTTP 200, returned 100 records, `info.totalrecordsperquery`=100
- size=500: HTTP 200, returned 100 records, `info.totalrecordsperquery`=100

## 6. Primary image URL — fetchable? (A7, C1 prep)

- object id=1429: primaryimageurl=`https://nrs.harvard.edu/urn-3:HUAM:79762_dynmc`, images[0].baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:79762_dynmc`
- object id=4586: primaryimageurl=`https://nrs.harvard.edu/urn-3:HUAM:DDC000086_dynmc`, images[0].baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:DDC000086_dynmc`
- object id=4615: primaryimageurl=`https://nrs.harvard.edu/urn-3:HUAM:DDC000085_dynmc`, images[0].baseimageurl=`https://nrs.harvard.edu/urn-3:HUAM:DDC000085_dynmc`

