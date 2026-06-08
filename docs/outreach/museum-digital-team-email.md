# Museum Digital-Team Cold Email

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-11.*

Tier 1 cold-email template for the digital / open-access teams at the
seven museums whose APIs p3a consumes. Send personally from the author's
address, staggered weekly per the plan's launch sequence: AIC (Week 1),
Rijksmuseum (Week 2), V&A (Week 3), Wellcome Collection (Week 4),
SMK (Week 5), Harvard Art Museums (Week 6), Smithsonian (Week 7).

**Note:** Harvard and the Smithsonian shipped after this template was
first drafted. Variants 6–7 below are written and ready, but whether to
send them as digital-team cold emails — vs. routing the Smithsonian
through its Tier-2 Open Access *showcase* slot — is the open cadence
decision flagged in `museum-outreach-plan.md`.

The pitch frame: **30-second video, GitHub link, no ask.**

The only unfilled placeholder is `{{video_url}}` — fill once the
museum-mode demo video is produced.

---

## Template (parameterized)

**Subject:** A $48 open-hardware device that plays the {{museum_short}} collection over IIIF

> Hi {{salutation}},
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from {{museum_full}} via {{program_name}} and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: {{protocol_note}}, decodes JPEGs on-device, no cloud middleman.
>
> {{museum_specific_line}}
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 1 — Art Institute of Chicago (Week 1)

**To:** open-access contact via [artic.edu/open-access](https://www.artic.edu/open-access)

**Subject:** A $48 open-hardware device that plays the AIC collection over IIIF

> Hi Open Access team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the Art Institute of Chicago via your Open Access program and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: hits your IIIF Image API directly with proper rate-limit handling, decodes JPEGs on-device, no cloud middleman.
>
> The AIC Open Access program is part of why p3a exists — the showcase you curate is a model the rest of the field follows. AIC is a first-class source in the device alongside the Rijksmuseum, the V&A, the Wellcome Collection, the Statens Museum for Kunst (SMK), the Harvard Art Museums, and the Smithsonian.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 2 — Rijksmuseum / Rijksstudio (Week 2)

**To:** Rijksstudio team via the Rijksmuseum API contact

**Subject:** A $48 open-hardware device playing the Rijksmuseum collection over IIIF

> Hi Rijksstudio team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the Rijksmuseum via your public APIs and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: walks your Linked-Art records to find the canonical IIIF image for each object, decodes JPEGs on-device, no cloud middleman.
>
> Rijksstudio's stance on creative reuse made the museum an obvious choice — the same openness that lets people remix the collection lets a $48 microcontroller serve it on a wall.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 3 — Victoria and Albert Museum (Week 3)

**To:** Digital team via [developers.vam.ac.uk](https://developers.vam.ac.uk/) contact

**Subject:** A $48 open-hardware device playing the V&A collection over IIIF

> Hi Digital team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the V&A via your Collections API and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: hits your IIIF endpoints directly, decodes JPEGs on-device, no cloud middleman.
>
> V&A sits alongside AIC, the Rijksmuseum, the Wellcome Collection, SMK, the Harvard Art Museums, and the Smithsonian as a first-class source on the device — your Collections API made the integration straightforward.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 4 — Wellcome Collection (Week 4)

**To:** Digital team via [wellcomecollection.org/developers](https://wellcomecollection.org/developers) contact

**Subject:** A $48 open-hardware device playing Wellcome Collection imagery over IIIF

> Hi Wellcome digital team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls images from the Wellcome Collection via your Catalogue / IIIF APIs and rotates through them like a tiny gallery on the desk. It's a firmware-level IIIF consumer: hits your IIIF endpoints directly, decodes JPEGs on-device, no cloud middleman.
>
> Wellcome's openly-licensed catalogue is what made it possible to drop the collection into the device alongside AIC, the Rijksmuseum, the V&A, SMK, the Harvard Art Museums, and the Smithsonian — Wellcome's stance on reuse is something I cite when I explain to people why p3a doesn't need permissions or accounts.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 5 — Statens Museum for Kunst / SMK (Week 5)

**To:** SMK Open / API team via [open.smk.dk](https://open.smk.dk/) contact

**Subject:** A $48 open-hardware device playing the SMK collection over IIIF

> Hi SMK Open team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the SMK collection via your public API and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: hits your IIIF endpoints directly, decodes JPEGs on-device, no cloud middleman.
>
> SMK Open's permissive licensing and clean API are what made adding the collection straightforward — the device now treats SMK as a peer of AIC, the Rijksmuseum, the V&A, the Wellcome Collection, the Harvard Art Museums, and the Smithsonian.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 6 — Harvard Art Museums (Week 6)

**To:** Harvard Art Museums API team via [harvardartmuseums.org/collections/api](https://harvardartmuseums.org/collections/api) (free API, key required)

**Subject:** A $48 open-hardware device that plays the Harvard Art Museums collection over IIIF

> Hi Harvard Art Museums API team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the Harvard Art Museums via your public API and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: it resolves your IDS image identifiers on-device (through the NRS→IDS redirect), decodes JPEGs on the chip, no cloud middleman.
>
> Harvard's API plus IDS imaging made the collection a natural fit — it now sits alongside the Art Institute of Chicago, the Rijksmuseum, the V&A, the Wellcome Collection, SMK, and the Smithsonian as a first-class source on the device.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Variant 7 — Smithsonian (Week 7)

**To:** Smithsonian Open Access team via [si.edu/openaccess](https://www.si.edu/openaccess) (API key via api.data.gov)

**Subject:** A $48 open-hardware device that plays the Smithsonian collection over IIIF

> Hi Smithsonian Open Access team,
>
> I built **p3a**, an open-hardware desktop art frame — a $48 ESP32-P4 board with a 4-inch 720×720 touchscreen — that pulls artwork from the Smithsonian via your Open Access API and rotates through it like a tiny gallery on the desk. It's a firmware-level IIIF consumer: it reads your Open Access metadata, fetches the IIIF image on-device, and decodes JPEGs on the chip — no cloud middleman.
>
> The breadth of Open Access across the Smithsonian's units is exactly what makes it compelling on a rotating frame — it now sits alongside the Art Institute of Chicago, the Rijksmuseum, the V&A, the Wellcome Collection, SMK, and the Harvard Art Museums as a first-class source.
>
> 30-second video: {{video_url}}
> Source / firmware: https://github.com/fabkury/p3a
>
> No ask — just thought your team might want to see it.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Notes on the choices

- **Subject line** leads with `$48` (concrete hook) + museum name
  (relevance) + `IIIF` (insider signal). Skips marketing words like
  "introducing," "showcase," "amazing."
- **First sentence** packs in the four facts that matter —
  open-hardware, price, their-API, what it does. If the email is
  skimmed and only this sentence reads, the recipient still has the
  full story.
- **"Firmware-level IIIF consumer" line** proves technical fluency
  without bragging. The specific protocol detail (rate-limit handling,
  Linked-Art walks) is what makes this email different from a marketing
  pitch.
- **One museum-specific sentence** shows the writer actually knows this
  institution rather than blasting the same email at three. Kept short
  on purpose — over-flattery reads as performative.
- **"No ask"** is stated literally because the plan calls for it and
  because it does the work — disarms the usual cold-email reflex of
  "what are they trying to sell me."
- **No unit offer** even though Tier 3 contemplates sending one. That
  belongs in a follow-up if they reply, not in the cold email itself.
- **No P.S.** — adds length without adding signal here.

## Send checklist

- [ ] Museum-mode video uploaded; URL plugged into all seven variants.
- [ ] Confirmed contact address on each museum's API/open-access page.
- [ ] Variants sent in order: AIC → Rijks → V&A → Wellcome → SMK →
      Harvard → Smithsonian (the last two pending the cadence decision
      in `museum-outreach-plan.md`), one per week, different days of the
      week to avoid coordinated-campaign optics on shared inboxes if any
      of these forward to the same syndicate.
- [ ] Logged send dates so any follow-up reply lands in the right
      thread.
