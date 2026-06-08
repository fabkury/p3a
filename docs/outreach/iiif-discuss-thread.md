# discuss.iiif.io Thread Draft

*Companion to `museum-outreach-plan.md` and `iiif-news-submission.md`.
Drafted 2026-05-11.*

Tier 1 outreach to the IIIF community forum at https://discuss.iiif.io.
Pairs with the IIIF Consortium news submission but is **not** a copy
of the same content — different shape (first-person, conversational,
ends in real questions) for a different surface.

Technical claims are taken from `docs/art-institutions/finalized-design.md`
and the `art_institution` component as of v0.10.0.

---

## Suggested title

**p3a: an embedded IIIF Image API client running on a $48 ESP32-P4 microcontroller**

Descriptive over clever — Discourse posts get found via search months
later, so keyword-loading the title pays off.

## Suggested category

**Implementations** if it exists; otherwise **Show and Tell** or the
general top-level IIIF category. Avoid posting in **IIIF Image API** —
that category is for protocol questions, not implementation
announcements; cross-posting an announcement there reads as off-topic.

## Suggested tags

`image-api`, `implementation`, `embedded`, `community-projects`

## Body

> Hi all — sharing an unusual implementation in case it's of interest.
>
> *[photo of the device displaying a painting]*
>
> **p3a** is an open-hardware desktop art frame — a Waveshare ESP32-P4 development board (~$48), 720×720 4-inch IPS, 32 MB PSRAM, 32 MB flash — and as of v0.10.0 it speaks the IIIF Image API natively from the firmware. It cycles through artwork from seven institutions — the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, the Statens Museum for Kunst (SMK), the Harvard Art Museums, and the Smithsonian — alongside pixel art from a community network and trending GIFs. Apache 2.0: https://github.com/fabkury/p3a
>
> A few protocol-level highlights, since this audience cares:
>
> - **Image API v2 for the pixels** — every image is ultimately requested as `…/full/!720,720/0/default.jpg` across all seven sources, and that confined-size request is what keeps decode tractable on the chip. No `info.json` negotiation yet (deferred — see questions below). Where the sources actually diverge is *discovery*: the Rijksmuseum needs a full Linked Art walk (below), the Harvard Art Museums sit behind an NRS→IDS redirect, the Smithsonian needs a User-Agent workaround past its WAF, and the rest return the IIIF id inline.
> - **Per-museum 429 handling.** A small per-museum cooldown table honors `Retry-After` and falls back to per-museum defaults. The browse UI runs in a LAN-side browser, and it reports its own 429s back to the device over a small REST endpoint so the per-IP rate-limit budget stays coherent across both clients. This matters for AIC's 60-req/min per-IP cap.
> - **Linked Art walk on-device** for the Rijksmuseum: HMO → VisualItem → DigitalObject → access_point, lazy-resolved at download time, with sentinel encoding for unresolved entries and a tombstone after 3 consecutive failures.
> - **Per-museum vault dedup** — a painting that appears in four AIC facets is stored once on the SD card. Vault paths are namespaced per museum, so Wellcome + SMK adapters drop in without touching the shared layer.
> - **JPEG-only rendition**, decoded by the ESP32-P4's hardware JPEG codec at 720×720.
>
> A few things I'd genuinely value input on:
>
> 1. **Does anyone know of prior firmware-level IIIF clients?** I haven't found one, but I'd rather hear about it than mis-claim a "first."
> 2. **`info.json`-aware rendition negotiation** — when does it pay off in practice? At a 720 px longest side, request-time `!720,720` has been Good Enough across the seven sources I'm consuming. Curious where the threshold sits for others.
> 3. **Aggregator sources.** I've since integrated the Smithsonian — an aggregator across many units, whose WAF needed a User-Agent workaround before it'd serve IIIF. Europeana and DPLA are still on my roadmap, and their resolution patterns seem to vary a lot more. Has anyone integrated those at the embedded level, or even thought about it?
>
> A 30-second video is at *[VIDEO URL]* and the source is at the link above. Happy to dig into any of the implementation choices.
>
> — Fabrício

---

## Notes on the choices

- **Image at the top** — Discourse posts with an opening image get
  dramatically more engagement; the IIIF crowd in particular wants to
  *see* the implementation, not just read about it.
- **Apache 2.0 + GitHub link in paragraph 1** — gets the headline
  answer to "is this open source" out before the reader has to scroll.
- **Three discussion hooks, not five** — more fragments the thread.
  The chosen three are real open questions from the design doc's
  "Future work" and "Field-observed fixes" sections, so any reply will
  land somewhere actionable.
- **The "first" claim is converted into a question.** Asserting "first"
  on a forum invites someone to disprove it; *asking* if anyone knows
  otherwise frames the same novelty as humility, which the IIIF
  community responds to better.
- **No mention of the museum-team email outreach** — keeping the
  channels separate keeps the forum thread feeling organic rather than
  a coordinated PR push.
- **No `@`-mentions of museum staff** — even if AIC/Rijks/V&A digital
  teams have accounts on discuss.iiif.io. The plan flagged mass-tagging
  as spam-adjacent; same logic applies here.
- **Sign-off as "Fabrício"** rather than full name — forum convention
  is first-name signoff, full name is in the profile.

## Stagger relative to the news email

The outreach plan groups news + discuss + Slack as one Tier 1 IIIF
burst, but **2–3 days between the cover email to Glen Robson and the
discuss.iiif.io post** is recommended in either order. Same-day firing
risks looking coordinated to staff who watch both surfaces. Forum post
first → email second is slightly cleaner ("I posted on discuss earlier
this week and thought you might also want to see it") because it gives
the email a soft hook.

## Send checklist

- [ ] Verify the actual category list on discuss.iiif.io and pick the
      closest one.
- [ ] Attach a real photo (the press kit V2 work covers this).
- [ ] Drop in the video URL if the museum-mode video is ready by then.
- [ ] Stagger 2–3 days from the IIIF Consortium cover email.
- [ ] Watch the thread for the first week — quick replies to questions
      multiply traction.
