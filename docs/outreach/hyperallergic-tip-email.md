# Hyperallergic Tip Email

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-11.*

Tier 1 outreach to the editorial team at Hyperallergic, scheduled for
Week 3 of the launch sequence (per the plan's stagger from Open
Culture in Week 4).

The pitch angle is **art democratization**, NOT technical fluency.
Hyperallergic's audience is art-curious editorial readers, not
technologists — IIIF, Linked Art, and ESP32-P4 internals belong in
the GitHub README, not the tip.

The only unfilled placeholder is `{{video_url}}` — fill once the
museum-mode demo video is produced.

---

## Cover email

**To:** `tips@hyperallergic.com` — verify against current Hyperallergic
contact page before sending.

**Subject:** A $48 open-hardware art frame that streams seven major museum collections to your wall

> Hi Hyperallergic team,
>
> A tip for the digital-art / maker-meets-museum beat: **p3a** is an open-source desktop art frame — a $48 4-inch touchscreen built on a Waveshare ESP32-P4 dev board — that rotates through real artwork from seven museums: the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, the Statens Museum for Kunst (SMK), the Harvard Art Museums, and the Smithsonian. A Vermeer at 9:00, a Picasso at 9:30, a V&A photograph at 10:00, a Wellcome anatomical engraving at 10:30, a Hammershøi from SMK at 11:00, a Sargent watercolor from Harvard at 11:30, a Smithsonian portrait at noon. No cloud account, no subscription, no museum-budget pricing — it pulls directly from each museum's own public APIs and runs entirely on the device.
>
> The angle that might travel: this is the first piece of consumer hardware I know of that treats the world's open museum collections as a first-class, default content source — the way a radio is a default for music. Museums have spent a decade opening their collections; this is what it looks like when an indie maker takes them at their word.
>
> It's Apache 2.0, and the whole project is one person.
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

- **Subject line leads with a concrete count (seven collections)**
  instead of a generic "art frame" pitch. Editors triage at speed;
  specifics earn the second glance, marketing-flavor headlines don't.
- **Concrete imagery in sentence 2** ("A Vermeer at 9:00, a Picasso at
  9:30…") gives the editor a scene to picture. Without a scene, a tip
  reads like a press release.
- **The framing paragraph is doing real work** — editors want a story
  handle, not a product. "Open museums + indie maker who took them at
  their word" is a frame they can run with as-is.
- **"The whole project is one person"** is a human-interest hook
  without being self-promotional. Hyperallergic covers solo-builder
  stories regularly.
- **No mention of IIIF, Linked Art, ESP32 specifics, or pixel art.**
  The audience doesn't care about protocol details, and pixel art —
  while p3a's primary identity — would dilute the museum hook here.
  The README on GitHub carries the full identity.
- **No "no ask" line** like the museum digital-team email — there *is*
  an implicit ask (run a story), and pretending otherwise would be
  misleading. The "happy to send a unit" is a soft, appropriate
  follow-up offer that fits editorial outreach norms.
- **No P.S.** — same logic as the IIIF news draft; adds length without
  adding signal here.
- **"Personal museum" framing avoided** in the body even though it was
  the plan's seed phrase. The plan also flags that Hyperallergic's
  audience shares cultural-heritage sensibilities, and "personal
  museum" reads close to "museum replacement" — a frame that
  cultural-heritage gatekeepers are allergic to. "Art frame" is the
  safer, more accurate framing.

## Send checklist

- [ ] Confirm `tips@hyperallergic.com` is the current intake address
      (or use the contact form on hyperallergic.com).
- [ ] Museum-mode video uploaded; URL plugged into the body.
- [ ] Quick search of Hyperallergic's recent coverage to confirm a
      similar device hasn't already been featured (and if so, name-
      drop the prior coverage as context).
- [ ] Sent in Week 3, ≥1 week separation from Week 4's Open Culture
      tip — same editorial cluster, simultaneous tips would look
      coordinated.
- [ ] Logged send date so any follow-up reply lands in the right
      thread.
