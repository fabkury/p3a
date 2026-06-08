# We Make Money Not Art Tip Email

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-16.*

Tier 2 outreach to Régine Debatty's We Make Money Not Art, scheduled
for Week 6 of the launch sequence — well after the IIIF community
wave so the email can lean on "this got covered in cultural-heritage
tech circles" as soft social proof.

The pitch angle is **tech-art / maker-engaging-cultural-institution**,
which is the niche Régine has covered for 20+ years. Differs from
Hyperallergic (art-democratization, art-curious general readers) and
Open Culture (open-content trifecta, open-licensing readers): WMMNA
treats objects like p3a as critical-practice artifacts, not consumer
products. The framing reflects that.

The only unfilled placeholder is `{{video_url}}` — fill once the
museum-mode demo video is produced.

---

## Cover email

**To:** `regine@we-make-money-not-art.com` — verify against the site's
contact page before sending. Régine usually replies from a personal
address.

**Subject:** Embedded IIIF on a $48 microcontroller — five museum collections playing on a handmade desktop frame

> Hi Régine,
>
> A tip in the maker-meets-cultural-institution vein you've been covering for a long time: **p3a** is an open-hardware desktop art frame — a $48 ESP32-P4 board, 4-inch 720×720 touchscreen — and at v0.10.0 the firmware speaks IIIF natively, pulling artwork from five museums: the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, and the Statens Museum for Kunst.
>
> The piece I think is worth a paragraph on WMMNA: museums have spent a decade publishing collections under open licenses and open IIIF APIs, mostly with the assumption that the consumers would be other institutions, scholars, or web designers. p3a is what it looks like when a solo maker decides that "open" should mean "open enough to embed in a $48 microcontroller running on a desk." It's not a startup product — there's nothing to buy, no service to sign up for, no cloud account in the loop. The firmware is Apache 2.0 and the hardware is off-the-shelf. The whole project is one person.
>
> A small thing I keep noticing: every museum in the set treats the openness of its collection as a value statement, but p3a is one of the few consumer-grade objects that actually performs that statement back at them — playing a Vermeer at 9:00, a Wellcome anatomical engraving at 10:30, a Hammershøi at 11:00, on a piece of hardware anyone can build over a weekend.
>
> 30-second video: {{video_url}}
> Source / docs: https://github.com/fabkury/p3a
>
> Happy to send a unit, photos, or jump on a call.
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Notes on the choices

- **Subject line** is the longest of any tip in this campaign, and
  deliberately so. WMMNA readers are scanning for substance; "embedded
  IIIF on a $48 microcontroller — five museum collections" reads as a
  niche-specific signal that the sender knows the beat.
- **First paragraph names the maker-meets-cultural-institution angle
  explicitly.** Régine has rejected pitches that don't read the room.
  Naming her beat back to her isn't sycophancy — it's signaling that
  you've read the blog before.
- **The middle paragraph is doing the conceptual work.** "Museums
  spent a decade opening their APIs; this is what happens when
  someone takes them at their word at the firmware level" is the
  critical-practice frame that fits WMMNA's editorial DNA.
- **"Performs the openness statement back at them"** — phrasing
  borrowed loosely from the criticality vocabulary the blog uses. Don't
  over-do this; one sentence in this register is plausible, three would
  read as parody.
- **Scene-painting (Vermeer / Wellcome / Hammershøi) at the end of the
  second paragraph**, not the first. WMMNA readers want the conceptual
  frame before the product detail; reverse from Hyperallergic.
- **"The whole project is one person"** is the human-interest hook —
  WMMNA covers solo-maker work consistently.
- **No mention of IIIF specifics beyond the protocol name** — Régine's
  audience overlaps with art critics, not protocol implementers; the
  GitHub link carries the technical depth.
- **No "no ask" line** — same logic as Hyperallergic / Open Culture;
  there is an implicit ask (run a tip), pretending otherwise is
  misleading.
- **"Happy to send a unit"** — WMMNA has historically featured
  hands-on photos of devices Régine has actually held. The offer
  meaningfully improves odds here vs. other editorial tips.

## Send checklist

- [ ] Confirm Régine's current email via the WMMNA contact page (or
      the site's About page).
- [ ] Museum-mode video uploaded; URL plugged into the body.
- [ ] Quick search of WMMNA's recent coverage to confirm a similar
      device hasn't already been featured (and if so, name-drop the
      prior coverage as context).
- [ ] Sent in Week 6, ≥1 week separation from any Hyperallergic /
      Open Culture / MCN sends — same broad editorial cluster.
- [ ] If covered, share the WMMNA piece with the museum digital teams
      you've already emailed; gives them a reason to amplify.
