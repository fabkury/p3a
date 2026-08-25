# IIIF-Discuss Mailing-List Email Draft

> ✅ **SUBMITTED 2026-08-25 — held in first-post moderation.** Fab
> sent the email to the list; Google Groups moderates new members'
> first posts, and a pending message is not visible to its author.
> Until it appears in the archive
> (https://groups.google.com/g/iiif-discuss) the send is not
> confirmed. **The 2–3-day Glen Robson stagger clock starts when the
> post becomes visible, not from submission.** If nothing appears
> within ~5–7 days, assume the pending message expired or was missed
> and ask the group owners (iiif-discuss+owners@googlegroups.com) or
> resend — pending queues on Google Groups silently drop messages
> after a while if no moderator acts.

*Companion to `museum-outreach-plan.md` and `iiif-news-submission.md`.
Drafted 2026-05-11 as a discuss.iiif.io forum thread; **reworked
2026-08-25 as a mailing-list email** after Fab joined the IIIF-Discuss
list (2026-08-24). Decisions locked that day: the email REPLACES the
forum post (same audience — do not send both), it goes out
**videoless**, with 1–2 device photos attached, and the AIC Cloudflare
block gets a brief factual mention only (no discussion question built
on it).*

Tier 1 outreach to the IIIF community. Pairs with the IIIF Consortium
news submission but is **not** a copy of the same content — different
shape (first-person, conversational, ends in real questions) for a
different surface.

Technical claims verified against the `art_institution` component and
`docs/art-institutions/finalized-design.md` as of firmware 1.2.0
(2026-08-25): nine museums, seven over IIIF, AIC disabled via
`unavailable_reason`, CMA/Mia as the two non-IIIF fixed-rendition
adapters.

---

## Subject line

**p3a: an embedded IIIF Image API client running on a $48 ESP32-P4 microcontroller**

Descriptive over clever — list archives get found via search months
later, so keyword-loading the subject pays off. No "[ANN]"-style
prefix unless the archive skim (see checklist) shows the list uses
one.

## Body

> Hi all,
>
> I built p3a, an open-hardware desktop art frame that speaks the IIIF
> Image API natively from microcontroller firmware. The hardware is a
> Waveshare ESP32-P4 development board (~$48) with a 4-inch 720x720
> IPS panel. No browser and no cloud middleman: the firmware makes the
> Image API requests and decodes the JPEGs on-chip. It rotates through
> open-access artwork from nine museums, including the Rijksmuseum,
> the V&A, the Wellcome Collection, SMK, the Harvard Art Museums, and
> the Smithsonian, alongside pixel art and GIFs from other sources.
>
> Source (Apache 2.0): https://github.com/fabkury/p3a
> Two photos of the device are attached.
>
> Some notes for this audience:
>
> - Seven of the nine sources are consumed over the IIIF Image API,
>   and every image is requested as .../full/!720,720/0/default.jpg.
>   That confined-size request is what makes decoding tractable on the
>   chip. The interesting divergence is discovery: the Rijksmuseum
>   needs a Linked Art walk (HMO -> VisualItem -> DigitalObject ->
>   access_point, resolved lazily at download time), Harvard sits
>   behind an NRS-to-IDS redirect, and the Smithsonian's WAF wants a
>   real User-Agent.
> - The Art Institute of Chicago was the first source I integrated,
>   but since August 2026 its IIIF host answers non-browser clients
>   with a Cloudflare challenge, so current firmware ships with AIC
>   disabled.
> - The two newest sources, Cleveland and Minneapolis, publish fixed
>   CDN renditions instead of IIIF. They work on this panel only
>   because their baked sizes happen to land near 720 px. IIIF turns
>   that coincidence into a guarantee, which is exactly why it is the
>   right protocol for a device like this.
> - Rate limits are handled per museum: a cooldown table honors
>   Retry-After, and the browse UI (which runs in a LAN browser)
>   reports its own 429s back to the device so the per-IP budget
>   stays coherent.
>
> Three questions for the list:
>
> 1. Has anyone seen a prior firmware-level IIIF client? I believe
>    this is the first, and I would like to know if I am wrong.
> 2. At a 720 px longest side, request-time !720,720 has been good
>    enough everywhere, so I skipped info.json negotiation entirely.
>    Where does it start to pay off in your experience?
> 3. I evaluated Europeana and DPLA and deferred both: Europeana's
>    Thumbnail API caps at 400 px and its provider-level image URLs
>    are too heterogeneous, and DPLA serves thumbnails only. If
>    someone knows a reliable path to mid-size renditions across
>    Europeana providers, I would like to hear it.
>
> Happy to dig into any of the implementation choices.
>
> Fabrício

---

## Notes on the choices

- **Revised 2026-08-25 (later) per Fab:** no em dashes anywhere in
  the body (reads as an AI-text tell these days), self-confident
  register instead of the mildly apologetic one ("I built p3a" /
  "I believe this is the first"), and shorter overall. Cut in the
  tightening: vault dedup, the hardware JPEG codec, the
  sentinel/tombstone detail, the full nine-museum enumeration
  (six named + "nine museums, including"; AIC/CMA/Mia surface in
  their own bullets anyway).
- **Mailing-list format, not Discourse.** Plain text first: no
  markdown syntax load-bearing anywhere (the dashes and numbers read
  fine as raw text), URLs on their own visual lines, ASCII arrows
  (`->`) in the Linked Art chain so nothing depends on the reader's
  client rendering Unicode or rich text.
- **Photos attached instead of an inline hero image** (decided
  2026-08-25). Two attachments from the 08-16 device shoot — the IIIF
  crowd wants to *see* the implementation, and Google Groups renders
  attachments fine. Keep combined size modest (~1 MB) so no one's
  digest chokes.
- **Videoless by decision** (2026-08-25). A professional listserv is
  the venue where the museum-mode video matters least;
  implementation-first text plus photos carries it. If the video
  exists by send time anyway, a link can be added — but do not hold
  the email for it.
- **Apache 2.0 + GitHub link in paragraph 1** — gets the headline
  answer to "is this open source" out before the reader has to
  scroll.
- **AIC gets one factual sentence, no question** (decided 2026-08-25).
  Accurate about what currently works without building a
  naming-and-shaming thread around a member institution. If replies
  pick the thread up, expand there — a reply is a better register for
  that conversation than the announcement itself.
- **The CMA/Mia non-IIIF bullet is quiet advocacy.** "Coincidence into
  a guarantee" makes the case for IIIF adoption from the consumer
  side without lecturing anyone.
- **Three discussion hooks, not five** — more fragments the thread.
  All three are real open questions: the "first" claim stated
  confidently but left falsifiable ("I believe this is the first,
  and I would like to know if I am wrong"), the info.json deferral
  from the design doc, and the Europeana/DPLA question straight from
  the 2026-08 content-sources survey
  (`docs/content-sources-survey.md`) — Europeana IIIF people are on
  this list and can actually answer it.
- **No social-proof numbers** — per the plan's tiered policy: citing
  Reddit karma in professional listservs reads as marketing.
- **No mention of the museum-team email outreach** — keeping the
  channels separate keeps the post feeling organic rather than a
  coordinated PR push.
- **Sign-off as "Fabrício"** — list convention is first-name signoff;
  the full name is in the From header.

## Stagger relative to the news email

The outreach plan groups news + community post + Slack as one Tier 1
IIIF burst, but **2–3 days between this list email and the cover
email to Glen Robson** (see `iiif-news-submission.md`) is recommended.
Same-day firing risks looking coordinated to staff who watch both
surfaces. List email first → news email second is slightly cleaner
("I posted on IIIF-Discuss earlier this week and thought you might
also want to see it") because it gives the news email a soft hook.

Sending this email starts the museum-track Tier 1 clock: the
IIIF-community pickup is the social proof the later museum-team
emails and editorial tips lean on, so be ready to continue the
cascade (see `museum-outreach-plan.md` launch sequence).

## Send checklist

- [x] ~~Skim the list archive~~ **DONE 2026-08-25** (in-browser, 30
      threads Mar–Aug 2026 + 4 full threads). Findings: tool and
      implementation announcements from individuals/vendors are a
      normal, accepted genre (serverless-iiif v8.0.0, Universal
      Viewer 4.4, Adno, Axiell Level-0 question); no forum mirroring;
      subjects are plain-descriptive, no `[ANN]` prefixes; expect
      quiet reception (threads get 0–4 replies, 24–42 web views —
      value is inbox reach + archive, not thread volume). Norm
      observed: posts are link-only, nobody attaches images —
      attaching still fine on Google Groups, but keep it small.
- [ ] Pick two photos from the 2026-08-16 device shoot (one painting
      on screen, one context shot); downscale to ~1 MB combined.
- [ ] Send as plain text (or minimal HTML) from the address
      subscribed to the list.
- [ ] Stagger: this email first, Glen Robson cover email 2–3 days
      later.
- [ ] Watch replies for the first week — quick answers to questions
      multiply traction; the AIC topic in particular may resurface in
      replies and deserves a thoughtful, non-adversarial response.
