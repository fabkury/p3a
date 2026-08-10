# Submission email — Code4Lib Journal

- **STATUS: SENT 2026-08-10, 11:16 AM ET**, from fab@kury.dev to
  c4ljournal@gmail.com. Fab tightened the draft before sending — the
  as-sent version is archived at the bottom of this file. Per the
  journal's guidelines, expect a provisional decision within ~a month
  (by ~2026-09-10); if silence past mid-September, a short polite
  follow-up is reasonable.
- **To:** c4ljournal@gmail.com
- **From:** fab@kury.dev
- **Attachments:** `article.docx` (figures embedded), plus
  `figure-1-submission.png` and `figure-2-submission.png` (separate
  PNGs, ≤500 px wide per the article guidelines).
- Drafted 2026-08-10.

---

**Subject:** Article submission: "Writing an IIIF Client on a $40 Microcontroller"

Dear Code4Lib Journal editors,

I would like to submit the attached article for your consideration:
"Writing an IIIF Client on a $40 Microcontroller" — an implementation
report of about 4,500 words with two figures.

The article describes p3a, an open-source (Apache 2.0) desktop art
frame whose ESP32 firmware speaks the IIIF Image API natively and
pulls artwork from seven institutions: the Art Institute of Chicago,
the Rijksmuseum, the Victoria and Albert Museum, the Wellcome
Collection, the Statens Museum for Kunst, the Harvard Art Museums,
and the Smithsonian. Its central observation is an asymmetry: the
Image API delivered fully on its interoperability promise — one URL
template serves pixels from all seven institutions — while artwork
discovery above it required seven bespoke adapters against seven very
different search APIs. The article tours those seven discovery
layers, measures whether the IIIF Presentation API could have closed
the gap (with fresh endpoint probes of all seven institutions), and
closes with a short checklist of inexpensive things an institution
can do to make its collection usable by clients much smaller than a
browser.

As far as I could determine, no firmware-level IIIF client has been
described before — including in the Journal's back catalog, whose
IIIF coverage appears to be entirely server-side. I would welcome
correction on that point.

One transparency note up front: both the project and the article were
produced with substantial AI assistance, as detailed in the article's
acknowledgments; I have verified the technical claims and take sole
responsibility for the content.

Author: Fabrício Kury (github.com/fabkury) is a biomedical
informatician in New York City. He currently works with Medicare
claims data analytics at Sparx, Inc. Disclosure: I have no financial
interest in any product, service, or institution discussed; p3a and
Makapix Club are noncommercial open-source projects.

The attached DOCX embeds both figures for reading convenience;
separate PNG files sized per your guidelines are attached as well. I
am happy to adjust formatting and references (CSE) to house style.

Thank you for your consideration,

Fabrício Kury
fab@kury.dev

---

## As sent (2026-08-10, 11:16 AM ET) — verbatim

Subject: Article submission: "Writing an IIIF Client on a $40 Microcontroller"

Dear Code4Lib Journal editors,

I would like to submit the attached article for your consideration:
"Writing an IIIF Client on a $40 Microcontroller," an implementation
report of about 4,500 words with two figures.

The article describes p3a, an open-source (Apache 2.0,
github.com/fabkury/p3a) desktop art frame whose ESP32-P4 firmware
natively speaks the IIIF Image API and pulls artworks from seven
institutions. Its central observation is an asymmetry: the Image API
delivered fully on its interoperability promise, while artwork
discovery above it required seven bespoke adapters against seven very
different search APIs. The article tours those seven discovery layers,
measures whether the IIIF Presentation API could have closed the gap,
and offers a short checklist of inexpensive things an institution can
do to make its collection usable by clients much smaller than a
browser.

For transparency, both the project and the article were produced with
substantial AI assistance, as detailed in the article's
acknowledgments. I have verified all claims and take sole
responsibility for the content.

Author: Fabrício Kury (github.com/fabkury) is a biomedical
informatician in New York City. He currently works with Medicare
claims data analytics at Sparx, Inc.

Author email: fab@kury.dev

Author URL: github.com/fabkury, linkedin.com/in/fab-kury

Disclosure: I have no financial interest in any product, service, or
institution discussed; p3a and Makapix Club are noncommercial
open-source projects.

Thank you for your consideration,

Fabrício Kury
fab@kury.dev
