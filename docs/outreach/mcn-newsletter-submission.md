# MCN Newsletter Submission

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-16.*

Tier 2 outreach to the Museum Computer Network (MCN) newsletter,
scheduled for Week 8+ of the launch sequence. MCN's audience is
museum digital teams and museum-tech professionals — the bosses and
peers of the digital-team contacts already emailed in Weeks 3–7. By
the time MCN runs this, several of the museums named below should
already have seen the device through direct outreach.

**Framing.** Not a project announcement. The MCN newsletter is for
content that's useful to museum digital teams — implementation
patterns, reuse examples, "here's how someone built against our APIs
that we hadn't anticipated." The submission below is shaped as a
news-item blurb the editor can drop in with light copy-edits, plus
optional expanded paragraphs and a quote-pull.

Technical claims taken from `docs/art-institutions/finalized-design.md`
and the `art_institution` component as of v0.10.0.

The only unfilled placeholder is `{{video_url}}` — fill once the
museum-mode demo video is produced.

---

## Cover email

**To:** MCN editorial contact via the [MCN website contact form](https://mcn.edu/) — verify the current intake address before sending.

**Subject:** For the MCN newsletter: an embedded IIIF client reusing five museum collections

> Hi MCN editorial team,
>
> Submitting a community-implementation note for the newsletter. **p3a** is an open-hardware desktop art frame built on a $48 ESP32-P4 microcontroller; as of v0.10.0 its firmware embeds a IIIF Image API client and consumes the open-access APIs of five institutions — the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, and the Statens Museum for Kunst.
>
> I think it's a useful concrete data point for MCN readers because the device sits in an unusual category: not a museum-built product, not a startup's product, not a research prototype — a working consumer-grade object that exists because of the digital-access decisions museums have made over the last decade. The pitch for digital teams trying to justify continued open-access investment, more or less.
>
> Below is a drop-in blurb in newsletter style (~150 words), plus an extended paragraph and a quote-pull if you have space. Happy to adjust framing.
>
> 30-second video: {{video_url}}
> Source / docs (Apache 2.0): https://github.com/fabkury/p3a
>
> Best,
> Fabrício Kury
> pub@kury.dev

---

## Drop-in blurb (newsletter-ready, ~150 words)

> **A microcontroller IIIF client puts five museum collections on a $48 desktop frame**
>
> p3a, an open-source art-frame firmware released this spring, is one of the first consumer-scale objects to consume museum IIIF APIs at the firmware level. Built on the Waveshare ESP32-P4 development board (a 4-inch 720×720 touchscreen, ~$48 retail), the device cycles through artwork pulled directly from the Art Institute of Chicago, the Rijksmuseum, the Victoria and Albert Museum, the Wellcome Collection, and the Statens Museum for Kunst — no cloud account, no subscription, no intermediary service.
>
> The firmware speaks IIIF Image API v2 natively, performs on-device Linked Art resolution for Rijksmuseum objects, and respects per-museum rate limits. Released under Apache 2.0, it's the work of a single maker, Fabrício Kury. Source and documentation: https://github.com/fabkury/p3a

---

## Optional extended paragraph (if the newsletter has the space)

> Looked at from the museum-side, p3a is a useful data point in the
> long-running argument about whether open-access programs justify
> the engineering investment: it exists precisely because the five
> participating institutions made their collections programmatically
> reachable. None of them were consulted in advance. The device
> consumes their public APIs, respects their published rate limits,
> and surfaces their attribution and metadata to the end user. It is,
> in effect, the smallest possible affirmative answer to "does anyone
> actually use these APIs?"

---

## Quote-pull (for the sidebar / pull-quote treatment)

> "Museums have spent a decade publishing their collections under
> open licenses and open IIIF APIs. p3a is what it looks like when
> someone takes that work at its word at the firmware level — on a
> $48 device anyone can build."
>
> — Fabrício Kury, p3a

---

## Notes on the choices

- **Three nested deliverables (blurb / extended paragraph /
  pull-quote)** give the editor flexibility. Most newsletter
  submissions arrive as one undifferentiated wall of text; offering
  pre-formatted layers is a meaningful courtesy.
- **The drop-in blurb is genuinely drop-in.** Third-person, no first-
  person voice, no marketing-flavor words. The editor can publish it
  with one or two adjustments to match house style.
- **Cover email frames why this matters to MCN readers** — "useful
  for digital teams justifying open-access investment" — rather than
  "look at this cool project." MCN's audience is institutional; the
  pitch needs to give them ammunition for their internal arguments.
- **Linked Art on-device** mentioned in the blurb (one technical
  detail) because MCN readers will recognize it as non-trivial; more
  detail would crowd out the institutional angle.
- **"One of the first"** rather than "the first" — MCN readers are
  more likely than most to know of an obscure prior implementation,
  and a Consortium / library-tech reader who reads MCN might already
  have replied to the Code4Lib post by then.
- **Quote-pull is sharper than the blurb prose** — pull-quotes are
  load-bearing in newsletter layout, and editors swap them in when
  they need a sidebar.
- **"None of them were consulted in advance"** in the extended
  paragraph is doing real work: it's the proof that the open-access
  programs are accomplishing what they claim. Saying so plainly is
  flattering to digital teams without sounding sycophantic.
- **No "happy to send a unit"** — MCN's audience is institutional;
  shipping a unit to a newsletter editor adds friction without clear
  payoff. Different from Hyperallergic / WMMNA where a unit is part
  of the editorial reflex.

## Send checklist

- [ ] Confirm MCN newsletter intake address via the current website
      contact form.
- [ ] Museum-mode video uploaded; URL plugged into the cover.
- [ ] Sent in Week 8+, *after* the museum digital teams have all been
      contacted and ideally after at least one has acknowledged. MCN
      editorial may cross-reference; "the AIC team has already seen
      this" is a useful soft credential.
- [ ] If picked up, cite the MCN coverage in any subsequent Museums
      and the Web submission — the venues read each other.
