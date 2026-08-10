# Code4Lib Journal article — working folder

- **Idea:** full-length article for the Code4Lib Journal about p3a's
  firmware-level IIIF client ("Writing an IIIF Client on a $48
  Microcontroller" — working title, not final).
- **Started:** 2026-08-07. **Article drafted 2026-08-08; Fab's full
  review (tracked changes + 9 comments) merged 2026-08-10** — his voice
  edits are now the base text of `article.md`; all comments resolved
  (JPEG section rewritten evidence-based: 2/7 museums serve progressive
  JPEG → always-software; V&A probe mechanics clarified; drift =
  eviction + re-download, old file lingers till age-based cleanup;
  64×4,096 limits verified; "fourteen months" → "three months";
  UA email pub@ vs bio email fab@ split confirmed; short "I would
  welcome correction." restored). Next steps only after review:
  Fab's language pass, PNG rasterization of figure 2 (≤500 px), CSE
  reference formatting, then (if go) the proposal/abstract.
- **Process decision:** article first, proposal later. Fab wants to see
  the finished article before deciding whether to pitch the editors at
  all. Do not draft or send any proposal until the article is reviewed
  and the go/no-go decision is made.
- **Facts pinned during writing (2026-08-08):** hero photo artwork
  identified — AIC 45243, *Two Dancers*, Edgar Degas, c. 1893–98,
  pastel and charcoal, `is_public_domain: true`, image_id
  `1381bcfe-b5a9-777a-9c53-adafc8a3ec58`. Prior-art search re-run:
  still nothing (open web + the searches already noted). Board price
  $39.99, "under $60 all-in" BOM phrasing per decision sheet.
- **Predecessor:** `../code4lib-post.md` — the PARKED listserv draft
  (2026-06-23, do not send). Its technical spine is reusable; its
  mailing-list framing is not.

## Journal facts (verified 2026-08-07)

- Rolling submissions, no call-for-papers needed: proposals or full
  articles to `c4ljournal@gmail.com` or journal.code4lib.org/submit-proposal;
  provisional decision within ~a month.
- Length guidance: 1500–5000 words, "the right length for what is being
  covered." Clarity and utility over formality; between blog post and
  scholarly paper; section headings where they help; CSE style for
  references.
- Code samples and screenshots "where (and only where) useful." Images
  as separate PNG files, inline display ≤500 px wide.
- Author retains copyright; article published CC-BY (US). Code should
  carry an open-source license (p3a is Apache 2.0 — fine).
- Submission needs: abstract, author name + email, 1–2 sentence bio,
  disclosure of any financial interest in products/services discussed.

## Positioning (verified 2026-08-07)

Every IIIF article the Journal has published is server-side
(tiling/scaling, HTJ2K vs JPEG2000, manifest pipelines, viewers,
migration). No published article covers an IIIF *client*, and none
covers embedded/firmware anything. The article's lane: what the IIIF
ecosystem looks like from the consuming edge, on a device with no OS,
no browser stack, and 32 MB of RAM — seven institutions' worth of
real-world API behavior observed from one constrained client.

## Decision sheet (grilling completed + confirmed 2026-08-08)

1. **Process:** article first; proposal drafted only after Fab reviews
   the article. Nothing gets sent anywhere without his go.
2. **Thesis:** "one URL template, seven discovery layers" as spine;
   constrained-client lens as recurring seasoning; genre =
   implementation report.
3. **Structure:** 10 sections, ~4,300 words. Cut-lines: browse UI out
   except where thesis-serving (Rijks CORS/baked set list); pure-embedded
   war stories compressed to clauses, not stories.
4. **Presentation API section** built on `presentation-api-research.md`
   (probes 2026-08-07): Presentation could NOT have served as the
   discovery layer — adoption broken at 5+/7 museums, spec omits
   counts/offset/facets/search, 50–100× request cost. Bespoke-search +
   Image-API is the division of labor the specs themselves prescribe.
5. **Voice:** first-person "I", plain-spoken, dry wit only where the
   facts carry it, affection for the museums stated before any quirk.
   AI referenced concretely: "Cursor" = a variety of models (including
   Anthropic's) inside the Cursor IDE; "Claude Code" = primarily the
   newest Opus model available at the time.
6. **AI disclosure:** full — in the narrative (project built end-to-end
   with LLM assistance; an LLM surfaced IIIF itself) and in a
   methods/acknowledgments note for the article text (drafted with AI
   assistance; Fab revises and takes sole responsibility). No
   Cursor/Anthropic affiliation.
7. **Code:** at most ONE pseudo-code block — the AIC partitioning
   algorithm. All other code becomes prose or tables; the IIIF URL
   template appears inline.
8. **Figures:** hero photo `images/photos/p3a-museum-channel-5.jpg`
   (Degas in hand; identify artwork + museum for the caption);
   one architecture diagram (produced by Claude); adapter line counts
   as a table, not a chart.
9. **Title:** "Writing an IIIF Client on a $40 Microcontroller".
   Intro BOM: board $39.99 (Aug 2026) + microSD ~$10–15 + any 5 V USB-C
   supply — "under $60 all-in". "Microcontroller" kept; pedantry
   pre-empted with one clause stating exactly what the P4 is.
10. **Claims:** priority claim only in falsifiable humble form
    ("as far as I can determine… I would welcome correction"), search
    re-run at writing time; museums named frankly with affectionate
    framing; the Discussion checklist stays generic (no second
    finger-pointing).
11. **Author packet:** bio — "Fabrício Kury is a biomedical
    informatician in New York City. He currently works with Medicare
    claims data analytics at Sparx, Inc." URL: github.com/fabkury.
    Disclosure: nothing to disclose — p3a and Makapix Club are Apache
    2.0, noncommercial; plus the AI-assistance sentence.

## Files

- `README.md` — this file: status + decisions.
- `source-material.md` — gathered technical facts with code/doc
  references (input for the article; not the article).
- `presentation-api-research.md` — live-probe research memo: could IIIF
  Presentation have served as the discovery layer? (No — measured.)
- `article.md` — the article draft itself.
