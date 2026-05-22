# p3a Museum-Channels Outreach Plan

*Companion to `outreach-plan.md`. Compiled 2026-05-11; refreshed
2026-05-16 to reflect five shipped institutions (AIC, Rijksmuseum, V&A,
Wellcome Collection, SMK) and the agreed launch cadence.*

This plan adds venues unlocked by p3a's art-institution channels
(AIC, Rijksmuseum, V&A, Wellcome Collection, Statens Museum for Kunst).
Read alongside `outreach-plan.md` — that doc targets the pixel-art /
maker / ESP32 crowd; this one targets cultural-heritage tech, IIIF
specialists, library-tech, museum digital teams, and art-curious
editorial audiences. Combined, they form one campaign.

**Framing.** p3a's headline identity stays "pixel-art player." Museum
channels are the *hook* for reaching new audiences — not a rebrand.
Lead with the technical-cultural angle that fits each venue, not a
generic product pitch.

---

## What the new feature unlocks

The art-institution channels drop p3a into venue clusters that the
existing plan doesn't touch:

- **Cultural-heritage tech** — IIIF Consortium, Code4Lib, library-tech
  lists. They actively celebrate creative IIIF reuse; a microcontroller
  IIIF client is novel in that crowd.
- **Museum digital teams** — AIC, Rijksmuseum, V&A, Wellcome
  Collection, and SMK. The institutions whose APIs p3a consumes. Their
  digital teams reward reuse with feature posts to audiences of 100k–2M+
  followers. Wellcome's Wikimedia-partnership track record makes them
  especially likely to amplify; SMK's "SMK Open" program is the
  Scandinavian equivalent.
- **Art-tech editorial** — Hyperallergic, Open Culture, We Make Money
  Not Art, MCN. Cover digital-art-meets-DIY-hardware regularly.
- **Open-content / OpenGLAM advocates** — small but high-density.
  Reach via Bluesky/Mastodon tags and GitHub topics.

The strongest hook for any pitch in this track: **"Open-hardware $48
art frame, runs museums' own public IIIF APIs, no cloud middleman."**
Open APIs + open hardware + open source is the trifecta these audiences
want to celebrate.

---

## Tier 1 — One-shot launches (do these RIGHT, not fast)

### 1. IIIF community

Submit a news item to `iiif.io/news/`, post on `discuss.iiif.io`, and
if reachable, mention in the IIIF Slack.

- **Pitch angle:** "Embedded IIIF client on $48 ESP32-P4 hardware —
  possibly the first firmware-level IIIF consumer." Lead with the
  protocol-implementation novelty (rate-limit handling, Linked-Art
  walk, on-device JPEG decode at 720×720).
- **Why:** the IIIF community spreads novel implementations across
  every museum technologist in one shot. Implementation-first
  framing matches their interest.

### 2. Museum digital teams — direct email, staggered weekly

Don't fire all five the same week. Each email: 30-second video,
GitHub link, no ask. See `museum-digital-team-email.md` for all five
fully-drafted variants.

- **AIC first** — most-receptive of the five (they curate a public
  [API showcase](https://www.artic.edu/open-access)). Contact: their
  open-access / digital-experience team.
- **Rijksstudio next** — Rijksstudio is famously enthusiastic about
  creative reuse of the collection.
- **V&A third** — established institution with an active Collections
  API team.
- **Wellcome fourth** — Wellcome Collection's licensing and Wikimedia
  partnership make them an easy yes on creative reuse.
- **SMK fifth** — SMK Open is the Scandinavian peer of AIC's open-
  access program; smaller social footprint but high credibility in
  OpenGLAM circles.
- **Pitch angle:** "An open-hardware art frame plays your collection.
  Here's a video. No ask — just thought you'd want to see it."
- **Why:** combined social reach across the five is in the millions.
  A single feature post outperforms most other Tier 1 wins by an order
  of magnitude.

### 3. Code4Lib (`code4lib-l` + Code4Lib Journal)

Subscribe to the list for 2–3 weeks first (lurk → contribute). Cold
posting to listservs burns the venue permanently.

- **Pitch angle:** "A microcontroller IIIF client — rate-limit
  handling, Linked-Art walks at download time, on-device JPEG decode.
  Code is open." Implementation-first, not consumer-first.
- **Why:** very-high-density library-tech audience. If reception is
  warm, expand into a Code4Lib Journal article — they publish longer-
  form implementation pieces, and "writing an IIIF client for ESP32-
  P4" fits that format exactly.

### 4. Hyperallergic + Open Culture tips

Email separately, ~1 week apart, to avoid coordinated-campaign optics.

- **Pitch angle (Hyperallergic):** "DIY $48 device turns your wall
  into a personal museum that rotates through the Art Institute and
  Rijksmuseum." Art-democratization angle.
- **Pitch angle (Open Culture):** "Open-source firmware streams free
  cultural-heritage art via the museums' own public APIs." Open-
  content angle — their core beat.

---

## Tier 2 — Drip cadence (4–6 weeks, not all at once)

| Week | Action |
|------|--------|
| 1 | Bluesky + Mastodon launch wave: `#IIIF #openglam #digitalhumanities #culturalheritage`. Tag `@artic.bsky.social`, `@rijksmuseum.nl`, `@vam.ac.uk` if active. |
| 2 | r/museums — **DM a mod first**. |
| 3 | r/arthistory — same mod-first approach. Angle: "view masterworks rotating on hardware you can build for $48." |
| 4 | We Make Money Not Art tip — Régine Debatty's long-running tech-art blog covers exactly this beat. |
| 5 | MCN (Museum Computer Network) newsletter — submit via contact form. |
| 6 | Museums and the Web — submit news item to their digital-museums journal. |
| ongoing | When new artworks land on the device, post photo + tag the source museum. Recurring organic exposure beats one big push. |

Also low-cost / no-cost evergreen:

- **PR to [`mejackreed/awesome-iiif`](https://github.com/mejackreed/awesome-iiif)** — ask
  the maintainer first; the list currently skews server/viewer, so a
  hardware client may need sign-off before merge.
- **GitHub Topics** — add `iiif`, `openglam`, `museum-api`,
  `cultural-heritage`, `digital-art-frame` so museum-tech searchers
  surface the repo.
- **Smithsonian Open Access user showcase** — Smithsonian isn't yet a
  p3a source, but they actively spotlight projects using *any* museum
  open API. Worth a tip.
- **OpenGLAM Slack** — niche but on-target; needs an existing contact
  to join.

---

## Tier 3 — Where to spend the $0–$200

### Highest ROI: a "museum mode" 30-second demo video — $50–$100

Distinct from the pixel-art sizzle in `outreach-plan.md`. Slower cuts,
paintings and photographs, calm-tech aesthetic. Single asset that
drops into every museum-track pitch and the README's new Museums
section. **Essential** — almost every Tier 1 venue improves with it.

### If budget remains: a unit to a museum-tech personality — ~$60 + shipping

Smaller targeting field than the YouTuber spend in the original plan.
Candidates:

- An IIIF Consortium board member who blogs publicly
- A museum digital director with a personal newsletter or blog
- Curators at *Pioneer Works* or *Rhizome* (digital-art-organization
  side)

Cold-email first; offer no-strings unit.

### Combined-budget note

The existing `outreach-plan.md` Tier 3 already commits $50–$180 for
either YouTuber units or a pixel-art sizzle video. The museum-mode
video here is incremental ($50–$100). Realistically the combined Tier 3
budget runs $100–$340 if both plans are fully funded. If $200 is a
hard ceiling, prioritize the **museum-mode video** over the YouTuber
units
— the video pays off across both plans (it goes in README, social, and
museum-team emails), while YouTuber units only pay off in the maker
track.

### Don't spend on

- Boosted posts on art-tech blogs (banner ads on Hyperallergic /
  similar underperform)
- Conference booths at MCN, Museums and the Web — $1k+ minimum
- Generic "art frame" Instagram ads (audience too broad)

---

## Skip these

- **Mass-tagging museum accounts** — looks like spam; museum social
  teams quietly mute.
- **Posting to academic listservs without subscribing first** —
  etiquette violation on small lists like `code4lib-l`; permanent
  reputation hit.
- **Cold-DM'ing curators (vs. digital teams)** — curators don't care
  about web infrastructure; you'll get ignored and burn the channel.
- **Crossposting r/museums and r/arthistory same day** — mod overlap
  looks coordinated.
- **Pitching as a "museum replacement"** — frame as *companion to*
  museum-going, not substitute. Cultural-heritage gatekeepers are
  allergic to the former.

---

## New assets to prep first (Week 0)

Most Tier 1/2 venues in this track need things that don't exist yet:

1. **Museum-mode 30-second demo video** — see Tier 3 above. The single
   most leveraged new asset.
2. **README "Museums" section** — short callout: "p3a also plays
   paintings and photographs from the Art Institute of Chicago,
   Rijksmuseum, and Victoria and Albert Museum via their open IIIF
   APIs." Tucked into the existing Channels section, not a hero
   rewrite — preserves pixel-first identity.
3. **Press kit V2** — 3–5 high-res photos of the device displaying
   actual paintings and photographs. The existing pixel-art photos
   don't sell the museum angle.

---

## Suggested launch sequence

| Week | Action |
|------|--------|
| 0 | Prep assets: museum-mode video (see `museum-mode-video-brief.md`), confirm README Museums section, press kit V2. **Subscribe to `code4lib-l` now** so the 2–3 week lurk window is already burning down by the time the Week 3 post goes out. Add GitHub topics (`iiif`, `openglam`, `museum-api`, `cultural-heritage`). Open the "PR welcome?" conversation with `mejackreed/awesome-iiif`. |
| 1 | Produce the video (Fiverr turnaround ~5 business days). Plug `{{video_url}}` into all five drafts. |
| 2 | IIIF Consortium news submission + `discuss.iiif.io` post (forum first, email 2–3 days later). Bluesky/Mastodon launch wave. |
| 3 | AIC digital team email. Code4Lib mailing list post (after the lurk window). PR to `awesome-iiif` if maintainer signaled yes. |
| 4 | Rijksstudio team email. r/museums (mod-approved). |
| 5 | V&A digital team email. r/arthistory. |
| 6 | Wellcome Collection digital team email. We Make Money Not Art tip. |
| 7 | SMK / SMK Open team email. Hyperallergic tip. |
| 8+ | Open Culture tip (≥1 week after Hyperallergic). MCN newsletter submission. Museums and the Web. Smithsonian Open Access showcase. Tagging museums on Bluesky/Mastodon becomes ongoing pattern. |

The cadence is intentionally slower than the original 5-week plan:
five museums staggered weekly + IIIF community first + editorial press
last is ~8 weeks end-to-end. Resist the urge to collapse it — each
staggered email is a separate audience, and "this got picked up by the
IIIF community" is a real piece of social proof to lean on in the
later editorial pitches.

---

## Interleaving with `outreach-plan.md`

The two plans hit different audiences, so simultaneous launch is fine
in principle. But two practical adjustments:

- **Don't launch both Tier 1s the same week.** Stagger by ~1 week so
  organic Tier 1 attention from one doesn't get diluted by the other.
- **Bluesky/Mastodon posts can overlap** — different hashtag clouds
  reach disjoint audiences (`#ESP32 #pixelart` vs. `#IIIF #openglam`),
  no coordination optics.
- **r/museums (this plan) and r/pixelart (existing plan)** have zero
  mod overlap — safe to post same week if both are mod-approved.

---

## Follow-up artifacts to draft

- [x] IIIF Consortium news submission — `iiif-news-submission.md`
- [x] discuss.iiif.io thread — `iiif-discuss-thread.md`
- [x] Museum digital-team cold email template, all five museums —
      `museum-digital-team-email.md`
- [x] Hyperallergic tip email — `hyperallergic-tip-email.md`
- [x] Open Culture tip email — `open-culture-tip-email.md`
- [x] Museum-mode video brief (Fiverr) — `museum-mode-video-brief.md`
- [x] Code4Lib mailing list post — `code4lib-post.md`
- [x] We Make Money Not Art tip — `wmmna-tip-email.md`
- [x] MCN newsletter submission — `mcn-newsletter-submission.md`
- [x] Reusable Bluesky / Mastodon launch posts — `social-launch-posts.md`
- [ ] README "Museums" section — already present in `README.md` line 70;
      consider an "as seen on" line once IIIF community / press picks
      up.
- [ ] Press kit V2 — 3–5 high-res photos of the device displaying
      paintings and photographs. Use frames from the museum-mode video
      shoot as the source.

---

## Sources

- [IIIF Consortium](https://iiif.io/) — news + community
- [discuss.iiif.io](https://discuss.iiif.io/) — community forum
- [Art Institute of Chicago — Open Access / API](https://www.artic.edu/open-access)
- [Rijksstudio](https://www.rijksmuseum.nl/en/rijksstudio)
- [Victoria and Albert Museum — Collections API](https://developers.vam.ac.uk/)
- [Code4Lib](https://code4lib.org/) — mailing list and journal
- [Code4Lib Journal](https://journal.code4lib.org/)
- [`mejackreed/awesome-iiif`](https://github.com/mejackreed/awesome-iiif)
- [Hyperallergic](https://hyperallergic.com/)
- [Open Culture](https://www.openculture.com/)
- [We Make Money Not Art](https://we-make-money-not-art.com/)
- [Museum Computer Network](https://mcn.edu/)
- [Museums and the Web](https://mw.museumsandtheweb.com/)
- [Smithsonian Open Access](https://www.si.edu/openaccess)
