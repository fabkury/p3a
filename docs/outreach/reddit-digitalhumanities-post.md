# r/DigitalHumanities Post

*Drafted 2026-06-07. Incorporates the hooks that landed on the
r/MuseumPros launch (same day): the "hundreds of thousands of free
artworks" volume framing and the IIIF user-requested-canvas-size point.*

Museum-track Reddit post for r/DigitalHumanities — academic / standards
crowd. Distinct from r/MuseumPros (professionals) and the maker-track
`reddit-esp32-post.md`: this audience rewards the *protocol* novelty
(an IIIF Image API client on a microcontroller) and the
interoperability-in-practice angle, so the post leads with that and
carries two discussion questions. Covers all seven shipped institutions.

**Submission format.** Self/text post with the body below, **plus a
photo or the demo clip** as attached media. Lower self-promo risk than
r/MuseumPros because it's genuinely on-topic; a mod-DM is courteous but
optional.

**Link caveat.** DH generally allows a repo link for a real open-source
project, but check the sidebar first. If links are restricted (as on
r/MuseumPros), keep the URL out of the body and drop it in a comment
when someone asks.

---

## Title

IIIF on a microcontroller: a 4-inch screen that plays museum artworks straight from the Image API

## Body

> This is p3a, an open-source ESP32-P4 art player I built. The part this
> sub might find interesting: it's an IIIF Image API client running
> entirely on the microcontroller — no server, no proxy — driving a
> 4-inch 720×720 screen.
>
> It rotates through hundreds of thousands of free, openly-licensed
> artworks from seven institutions' IIIF endpoints:
>
> - Art Institute of Chicago
> - Rijksmuseum
> - Victoria and Albert Museum
> - Wellcome Collection
> - Statens Museum for Kunst
> - Harvard Art Museums
> - Smithsonian Institution
>
> What makes this tractable on a $48 microcontroller is IIIF's size
> parameter. The chip can't load a multi-hundred-megapixel master, but
> the Image API lets it ask for exactly what the panel needs —
> `…/full/!720,720/0/default.jpg` — and the server returns a right-sized
> JPEG. Without user-requested canvas sizes the files would simply be
> too big to decode on-device. A decade of standards work is what lets a
> gadget this small treat these collections as a content source at all.
>
> Where it stops being uniform is discovery. Reaching the image
> identifier differs sharply by institution: the Rijksmuseum needs a
> full Linked Art walk (HMO → VisualItem → DigitalObject → access
> point); Harvard hands you a base image URL behind an NRS→IDS redirect;
> others return the IIIF id inline. The pixels are solved; the graph
> around them isn't, quite.
>
> Two things I'd like other reads on:
>
> - Is that "Image-API-solved / discovery-still-bespoke" split what you
>   see across collections too?
> - Is a device like this — IIIF small enough to sit on a shelf — a
>   useful teaching object for *why* the standard matters?
>
> Open source (Apache 2.0): https://github.com/fabkury/p3a

## Submission notes

- **The sizing point and the discovery examples are the credibility
  core** for this crowd — be ready to go deeper in comments (info.json,
  compliance levels, Linked Art, the Smithsonian's WAF/User-Agent
  quirk). The two questions are the engagement bait; answer leads fast
  in the first hour.
- **≥1 week separation from the r/MuseumPros post (2026-06-07)** and
  from any Code4Lib / MCN / IIIF-discuss activity — overlapping IIIF
  audience; back-to-back reads as a coordinated campaign.
- **Don't overclaim interoperability.** The body centers the *sizing*
  benefit (which you've already vouched for publicly) and locates the
  heterogeneity in discovery — that's accurate. Only upgrade to a
  stronger "all seven honor the same `!w,h` request" claim if you've
  confirmed each server is IIIF compliance level ≥ 1; a level-0 server
  would reject `!720,720`, and this crowd knows it.
- **Attach a photo or the demo clip** — visual device, visual proof,
  softens the "is this an ad" read.
- No emoji; reflective register, not launch register.

## Send checklist

- [ ] Sidebar checked for link rules and flair; mod-DM if courtesy
      suggests it.
- [ ] Photo or demo clip attached.
- [ ] `!720,720` request shape and the seven names confirmed against
      current firmware.
- [ ] Posted ≥1 week after the r/MuseumPros launch and any
      Code4Lib / MCN / IIIF-discuss push.
- [ ] Posted a weekday morning US time; first reply ready within the hour.
