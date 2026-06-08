# Code4Lib Mailing List Post

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-16.*

Tier 1 outreach to `code4lib-l`, scheduled for Week 3 of the launch
sequence — *after* a 2–3 week lurk window so the post lands as a
community contribution rather than cold marketing. The plan explicitly
flags that posting without first subscribing and reading the list is
a permanent reputation hit.

**Framing.** Implementation report, not product announcement. The list
is strict about self-promotion; the right shape is "here's how I
implemented IIIF on a microcontroller, here's what surprised me, here
are questions the library-tech community would have better answers to
than I do."

Technical claims are taken from `docs/art-institutions/finalized-design.md`
and the `art_institution` component as of v0.10.0.

The only unfilled placeholder is `{{video_url}}` — fill once the
museum-mode demo video is produced. (The video helps but isn't
load-bearing here — text-first audience.)

---

## Suggested subject

**Implementation report: an embedded IIIF Image API client on an ESP32-P4 microcontroller**

Descriptive, keyword-rich, "implementation report" signals it's a
contribution not a pitch. Mailing list search indexes pick this up
later, so concrete terms ("ESP32-P4," "IIIF Image API") pay off.

## Body

> Hi all,
>
> Long-time reader, first-time poster. I've been lurking the last
> couple of weeks because I wanted to share an unusual IIIF
> implementation but wasn't sure if it'd be of interest. The
> recent thread on [pick one real recent thread topic before sending]
> made me think it might be.
>
> Quick background, since the device is uncommon for this list:
>
> **p3a** is an open-hardware desktop art frame — a Waveshare
> ESP32-P4 development board (~$48 retail) with a 4-inch 720×720
> touchscreen — and as of v0.10.0 it speaks the IIIF Image API
> natively from firmware. It currently pulls artwork from five
> institutions: the Art Institute of Chicago, the Rijksmuseum, the
> Victoria and Albert Museum, the Wellcome Collection, and the
> Statens Museum for Kunst. Apache 2.0:
> https://github.com/fabkury/p3a
>
> I'm writing because the implementation hit a few patterns that I
> suspect the list has informed opinions on, and because — as far as
> I can tell — there isn't an existing firmware-level IIIF client to
> learn from. The questions at the end of this post are real; I'd
> genuinely value pointers.
>
> **What it does, mechanically:**
>
> - Each museum is implemented as a per-museum adapter (refresh loop,
>   IIIF URL builder, optional resolver). The adapters share a
>   rate-limit cooldown table and a per-museum vault on the SD card.
> - All image requests are `…/full/!720,720/0/default.jpg`. No
>   `info.json` round-trips. The ESP32-P4 has a hardware JPEG codec
>   so JPEG-only is a deliberate constraint.
> - Per-museum 429 handling: cooldown table honors `Retry-After`,
>   falls back to per-museum defaults. The LAN-side browse UI reports
>   its own 429s back to the device over a small REST endpoint so the
>   per-IP rate budget stays coherent (mostly mattering for AIC's
>   60-req/min cap).
> - For the Rijksmuseum, the 3-hop Linked Art walk
>   (HMO → VisualItem → DigitalObject → access_point) runs on the
>   device at download time, with sentinel encoding for unresolved
>   entries and a tombstone after 3 consecutive failures.
> - Per-museum vault dedup — a painting that appears in four AIC
>   facets is stored once.
>
> All in 32 MB of PSRAM.
>
> **Things I'd value input on, in decreasing order of "this is
> probably a stupid question":**
>
> 1. **Has anyone seen a prior firmware-level IIIF client?** I'd
>    rather hear "yes, in 2017, here's the link" than mis-claim a
>    first. I've searched discuss.iiif.io and the Consortium's news
>    archive without finding one, but absence of evidence etc.
>
> 2. **`info.json` negotiation at this resolution.** At 720 px
>    longest side, request-time `!720,720` has been Good Enough across
>    the five museums I'm consuming. Where does negotiation start
>    paying off in your experience — only when stitching from larger
>    masters, or are there server-side reasons I'm missing?
>
> 3. **Aggregator sources (Europeana, DPLA, Smithsonian).** They're
>    on my roadmap. The resolution / licensing / IIIF-availability
>    patterns seem to vary considerably more than the museums I'm
>    currently consuming. Has anyone built against these and run into
>    things I should know before I start?
>
> 4. **Code4Lib Journal angle:** if the implementation seems worth
>    writing up in more depth — "Writing an IIIF Client on a $48
>    Microcontroller," roughly — would the Journal be interested? I
>    don't want to submit cold if the topic's a poor fit.
>
> A 30-second video of the device cycling through the five
> collections is at {{video_url}}, source at the link above.
>
> Happy to dig into any of the design choices on-list or off.
>
> — Fabrício Kury
>   pub@kury.dev

---

## Notes on the choices

- **"Long-time reader, first-time poster"** + reference to a recent
  thread is the standard mailing-list-etiquette signal that this isn't
  a cold-blast. Replace `[pick one real recent thread topic before
  sending]` with a real thread you read during the lurk window — this
  is the highest-leverage line in the post.
- **Implementation-report framing** is load-bearing. The list shuts
  out "look at my project" posts; opens to "here's how I implemented
  X, the community has better answers than I do."
- **Apache 2.0 + GitHub link in paragraph 2** — gets the openness
  signal in before the technical content; license-first matters to a
  library-tech audience that watches licensing closely.
- **The questions are real, ordered by humility.** "Has anyone seen
  a prior implementation" first, because asserting "first" on this
  list will earn a polite but firm correction; asking for prior art
  pre-empts it.
- **Question 4 is the Journal soft-pitch.** Per the plan, if the post
  gets warm reception, the Code4Lib Journal is the natural follow-up —
  longer-form implementation pieces are exactly their format. Asking
  is more graceful than submitting cold.
- **No "no ask"** — the implicit ask is genuine input on the four
  questions; pretending otherwise reads as false-modesty.
- **No bulleted "highlights" / no claim to novelty in the headline.**
  Code4Lib readers will judge the implementation on its specifics, not
  on framing.
- **Video link last, optional.** Text-first audience; the video helps
  but isn't the load-bearing asset for this venue (unlike Hyperallergic
  or museum-team emails).
- **First-name signoff** matches list convention; full name and email
  in the signature line.

## Stagger relative to the IIIF Tier 1

The IIIF Consortium news / `discuss.iiif.io` thread goes out in Week 2
of the agreed cadence. This Code4Lib post lands in Week 3. The two
audiences overlap modestly (some Code4Lib readers are also on
discuss.iiif.io), but the framings are different enough — Code4Lib
gets a library-tech implementation report; discuss.iiif.io gets a
protocol-focused show-and-tell — that running both is fine. Don't
cross-link, though, or it looks like a coordinated push.

## Send checklist

- [ ] Confirm subscription to `code4lib-l` is active and you've been
      reading for 2–3 weeks.
- [ ] Replace the `[pick one real recent thread topic]` placeholder
      with a real thread reference.
- [ ] Museum-mode video uploaded; URL plugged in (optional but helps).
- [ ] Send during a typical list-traffic window (Tue–Thu morning US
      time tends to get the best engagement).
- [ ] If the thread takes off, draft a Code4Lib Journal pitch
      separately — don't try to convert the mailing-list thread itself
      into a publication submission.
- [ ] Watch the list for the first week — quick technical replies to
      questions multiply traction; silence after Day 1 means the post
      didn't land and a follow-up bump won't help.
