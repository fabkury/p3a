# IIIF Consortium News Submission

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-11.*

Tier 1 outreach to the IIIF Consortium. Note: **iiif.io/news has no
public submission form** — posts are written by Consortium staff. The
right shape is a polite email to a staff member with a drop-in blurb
they can publish if they like.

Technical claims below are taken from `docs/art-institutions/finalized-design.md`
and the `art_institution` component as of v0.10.0.

---

## Cover email

**To:** Glen Robson `<glen@iiif.io>` (IIIF Technical Coordinator) — most
appropriate target for a technical-implementation note. CC `info@iiif.io`
if no reply in 2 weeks.

**Subject:** For iiif.io/news: a microcontroller IIIF client (p3a, ESP32-P4)

> Dear IIIF Consortium,
>
> I'm writing to suggest a community-implementation news item for iiif.io/news. **p3a** is an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — and at v0.10.0 it is, as far as I'm aware, the first firmware-level IIIF Image API consumer: implemented in C against ESP-IDF, decoding JPEGs on-device, with per-museum 429 handling and (for the Rijksmuseum) an on-device Linked Art walk.
>
> It currently plays artwork from seven institutions: the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, the Statens Museum for Kunst (SMK), the Harvard Art Museums, and the Smithsonian. Apache 2.0.
>
> Repository: https://github.com/fabkury/p3a
>
> I've drafted a suggested news-item blurb below in the style of recent posts. If iiif.io/news isn't the right venue, I'd appreciate any pointer — community blog, upcoming community call, anywhere the implementation might be of interest.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Drop-in blurb (matches the iiif.io/news house style)

> **An Embedded IIIF Client on a $48 ESP32-P4 Microcontroller**
>
> p3a, an open-hardware desktop art frame built on the Waveshare ESP32-P4 development board, is an embedded IIIF client that displays artwork pulled directly from the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, the Statens Museum for Kunst (SMK), the Harvard Art Museums, and the Smithsonian on a 720×720 4-inch screen. The firmware speaks IIIF Image API v2 natively, performs Linked Art resolution on-device for Rijksmuseum objects, and handles per-museum rate limits — all in 32 MB of PSRAM. The project is open-source (Apache 2.0) and may be the first IIIF client to run at the firmware level.
>
> [Read more →](https://github.com/fabkury/p3a)

---

## Supporting facts (for staff to quote if they expand)

- **Museums covered (v0.10.0):** Art Institute of Chicago, Rijksmuseum,
  Victoria and Albert Museum, Wellcome Collection, Statens Museum for
  Kunst (SMK), Harvard Art Museums, and the Smithsonian — seven
  institutions across the US (Art Institute of Chicago, Harvard Art
  Museums, Smithsonian), the Netherlands, the UK (general + medical-
  humanities), and Denmark.
- **Hardware:** Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, 720×720 IPS,
  32 MB PSRAM, 32 MB flash, ~$48 retail.
- **Image rendition:** firmware requests
  `…/full/!720,720/0/default.jpg`; every source in the set returns a
  right-sized JPEG, with no `info.json` round-trip.
- **Discovery is per-source:** reaching the IIIF image identifier
  differs by institution — the Rijksmuseum needs a Linked Art walk
  (detailed below), the Harvard Art Museums resolve via an NRS→IDS
  redirect, the Smithsonian needs a User-Agent workaround past its WAF,
  and the rest return the IIIF id inline.
- **Rate limiting:** per-museum cooldown table, honors `Retry-After`,
  with browser→device 429 reporting so the per-IP budget (notably
  AIC's 60 req/min) is shared coherently between the device and the
  LAN-side browse UI.
- **Rijksmuseum integration:** the 3-hop Linked Art walk
  (HMO → VisualItem → DigitalObject → access_point) runs on the device
  at download time, with sentinel encoding for unresolved entries and
  a tombstone after 3 consecutive failures.
- **Vault:** per-museum, deduplicated by IIIF key — a painting that
  appears in four AIC facets is stored once.
- **License:** Apache 2.0.
- **Author:** Fabrício Kury (pub@kury.dev).

---

## Notes on the choices

- **Hedged "first":** "As far as I'm aware" / "may be the first" —
  claiming an absolute first is hard to defend and easy for a
  Consortium reader to disprove from one prior project they happen to
  know. Hedging keeps the novelty claim while staying professional.
- **Cover email's last paragraph** offers an off-ramp ("if iiif.io/news
  isn't the right venue"). Converts a refusal into a referral instead
  of a dead end.
- **Drop-in blurb is fully self-contained** — the seven-museum list, one
  hook (Linked Art on-device), one constraint (32 MB PSRAM), license,
  link. Matches the 2–3 sentence length of recent posts.
- **No screenshot or video attached** — the cover is a cold email;
  attachments lower deliverability. The video belongs in any follow-up
  reply where they ask for more.
- **Single technical headline detail** in the blurb (Linked Art
  on-device) — picking one strong specific detail beats a list of three
  weak ones.
- **Glen Robson chosen** over the generic `info@iiif.io` because the
  message is technical-implementation in nature, and the Technical
  Coordinator role is the obvious fit. Generic mailbox is the fallback.

## Send checklist

- [ ] Confirm Glen Robson's current email via iiif.io staff page.
- [ ] Send cover email + blurb.
- [ ] If no reply in 2 weeks, follow up CC'ing `info@iiif.io`.
- [ ] If declined for iiif.io/news, ask about the IIIF community call
      or community blog as a fallback.
- [ ] Pair with a `discuss.iiif.io` thread the same week — different
      audience inside the same community, doesn't double-tap the
      consortium staff.
