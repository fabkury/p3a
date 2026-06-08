# Bluesky / Mastodon Launch Posts

*Companion to `museum-outreach-plan.md`. Drafted 2026-05-16.*

Tier 1/2 social-launch copy for the museum-channels campaign, scoped
for **Bluesky and Mastodon** specifically. (Pixel-art-track social copy
lives separately and uses `#ESP32 #pixelart`; per the parent plan the
hashtag clouds are disjoint enough that the two tracks can post the
same week without coordination optics.)

**Cadence.** Drip 2–3 posts per week over the first 2–3 weeks of the
launch sequence — not all in one day. Pin Post 1 (launch) on your
Bluesky and Mastodon profiles for the duration of the campaign.

**Char budget.** Each post below is sized for **Bluesky (300 chars)**;
Mastodon (500 chars) variants are noted where a longer body would
add value. Image attachment is mandatory on each — text-only posts in
this hashtag cloud get a fraction of the engagement.

**Hashtag cloud.** `#IIIF` `#openglam` are the on-target tags this
audience subscribes to. `#culturalheritage` `#digitalhumanities`
`#openaccess` are broader. Use 2–3 tags per post (any more reads as
spam-adjacent).

The only unfilled placeholder is `{{video_url}}` (Post 7) — fill once
the museum-mode demo video is produced. Other posts attach a photo,
not the video.

---

## Post 1 — Launch (Week 2, Tuesday)

**Image:** Device on a desk displaying a recognizable painting (a
Vermeer from the Rijksmuseum is the strongest opener — instantly
readable at thumbnail size).

**Body** (294 chars):

> New: **p3a** v0.10.0 plays artwork from five museums — Art
> Institute of Chicago, Rijksmuseum, V&A, Wellcome Collection, and
> SMK — on a $48 ESP32-P4 desktop frame. Firmware speaks IIIF
> natively. Open hardware, open source (Apache 2.0).
>
> https://github.com/fabkury/p3a
>
> #IIIF #openglam

**Mastodon variant (445 chars):** add a sentence before the link:
*"No cloud, no subscription, no middleware — it pulls directly from
each museum's public IIIF endpoints and runs entirely on-device."*

---

## Post 2 — Art Institute of Chicago (Week 2, Friday)

**Image:** A recognizable AIC piece on the device — Hopper's
*Nighthawks*, Caillebotte's *Paris Street; Rainy Day*, or a Monet
*Water Lilies* panel all photograph well.

**Body** (282 chars):

> The Art Institute of Chicago's Open Access program is what makes
> this possible: full IIIF endpoints, generous rate limits, clear
> licensing. Here's *Nighthawks* on a $48 ESP32-P4 desktop frame
> running open-source firmware.
>
> https://github.com/fabkury/p3a
>
> #IIIF #openglam

---

## Post 3 — Rijksmuseum + Linked Art note (Week 2, Sunday)

**Image:** Vermeer's *Milkmaid* or Rembrandt's *Night Watch*
fragment on the device.

**Body** (296 chars):

> Rijksmuseum on p3a: the firmware does the full Linked Art walk
> on-device — HMO → VisualItem → DigitalObject → access_point —
> in 32 MB of PSRAM, then streams the IIIF image at 720×720. The
> Vermeer is *The Milkmaid*.
>
> https://github.com/fabkury/p3a
>
> #IIIF #LinkedArt #openglam

**Note:** This post leans technical — the LinkedArt tag is small but
high-density. Expect modest reach but high-quality engagement.

---

## Post 4 — Victoria and Albert Museum (Week 3, Tuesday)

**Image:** A Julia Margaret Cameron photograph or a William Morris
textile pattern on the device.

**Body** (276 chars):

> Victoria and Albert Museum on p3a — a Julia Margaret Cameron
> portrait this morning. The V&A's Collections API + IIIF
> endpoints are clean and well-documented; the integration took
> about a weekend.
>
> https://github.com/fabkury/p3a
>
> #IIIF #openglam #VAM

---

## Post 5 — Wellcome Collection (Week 3, Friday)

**Image:** A Vesalius anatomical plate, a botanical illustration, or
a Hooke microscopy image on the device.

**Body** (289 chars):

> Wellcome Collection on p3a. Vesalius at 11:00, a botanical plate
> at 11:30, a Hooke microscopy image at 12:00. Wellcome's
> open-license stance and IIIF infrastructure made this one of the
> smoothest integrations of the five.
>
> https://github.com/fabkury/p3a
>
> #IIIF #openglam

---

## Post 6 — SMK / Statens Museum for Kunst (Week 3, Sunday)

**Image:** Hammershøi's *Interior with a Young Man Reading* or
Krøyer's *Summer Evening on Skagen Beach* on the device.

**Body** (271 chars):

> SMK on p3a. A Hammershøi interior at golden hour on a $48
> open-hardware desktop frame. SMK Open's API + permissive
> licensing made the Danish national collection a first-class
> source.
>
> https://github.com/fabkury/p3a
>
> #IIIF #openglam #SMKOpen

---

## Post 7 — The trifecta / video drop (Week 4, Tuesday)

**Image / video:** The 9:16 vertical cut of the museum-mode demo
video. This is the post the video is *for* on social.

**Body** (290 chars):

> Open hardware + open-source firmware + open cultural APIs.
> p3a is the trifecta on a $48 desktop art frame, playing
> the Art Institute, Rijksmuseum, V&A, Wellcome, and SMK
> collections via their own IIIF endpoints. 30 seconds:
>
> {{video_url}}
>
> #IIIF #openglam #openhardware

**Mastodon variant:** the longer body has room for: *"No cloud
account in the loop. The whole project is one person. Apache 2.0."*

---

## Post 8 — "What surprised me" reflective post (Week 4, Friday)

**Image:** A side-angle shot of the device on a shelf with everyday
objects (the "in-context" frame from the video shoot).

**Body** (286 chars):

> Building p3a's IIIF client, what surprised me: the five museums
> have wildly different APIs but their IIIF endpoints are nearly
> interchangeable. Same `…/full/!720,720/0/default.jpg` request,
> same result, on a $48 microcontroller. That's what an open
> standard buys you.
>
> #IIIF #openglam

---

## Notes on the choices

- **Image is mandatory on every post.** Bluesky and Mastodon both
  reward image attachments dramatically in this hashtag cloud. A
  text-only post in `#IIIF #openglam` gets ~1/10 the reach.
- **No museum @-mentions in the post bodies.** The parent plan
  flagged mass-tagging as spam-adjacent; same logic applies. If the
  museums have official Bluesky / Mastodon accounts that are *active*,
  use a *reply* to your own post to tag them ("cc @artic.bsky.social
  if you want to see it") — that's organic, not spam.
- **GitHub link in every museum-specific post**, not just the launch.
  Each post lands in front of a different sub-slice of the IIIF /
  openglam audience; assuming they saw the link in Post 1 is wrong.
- **No emoji.** The IIIF / openglam crowd skews professional /
  curatorial; emoji-heavy posts read as marketing.
- **No threading.** Bluesky and Mastodon threads underperform vs.
  pinned standalone posts in this audience. Each post should stand
  alone.
- **Two-tag floor, three-tag ceiling.** Six hashtags reads as
  spam-adjacent; one hashtag underuses the discovery channel.
- **Cadence (Tue / Fri / Sun) targets peak engagement windows in this
  audience cluster** — academic and museum-tech folks check social on
  midweek and weekend mornings, not Mondays.
- **Post 8 is deliberately last.** Reflective / what-I-learned posts
  perform well *after* an audience has seen the device a few times;
  leading with it would feel premature.
- **Pin Post 1.** A pinned launch post lets late arrivals catch up
  without scrolling. Unpin after Week 6 to avoid stale-pin smell.

## What's NOT in this set

- **No "we're on Hacker News!" / "we got featured on Hyperallergic!"
  reaction posts.** Those go in a separate "amplification" set,
  written after the actual coverage lands; pre-drafting them now would
  invite jinxing the launches.
- **No #ESP32 / #pixelart posts.** Those belong in the pixel-art track
  (`outreach-plan.md`) and would dilute the openglam-targeted reach
  here.
- **No Twitter/X copy.** Per the parent plan, Lospec and OpenGLAM
  communities are concentrated on Bluesky/Mastodon, not X. Skip.

## Send checklist

- [ ] Verify Bluesky and Mastodon accounts are set up with profile
      photo, bio, and pinned URL.
- [ ] Capture all 8 photos before posting Post 1 (no scrambling
      mid-campaign).
- [ ] Confirm the Bluesky char count after pasting — Bluesky counts
      links as ~22 chars, so the headroom in each draft above already
      accounts for it.
- [ ] Schedule with a tool (Buffer / Hypefury / Mastodon's built-in
      scheduler) so the cadence holds without you babysitting.
- [ ] Watch the first 24 hours of each post — replies to comments and
      reposts compound engagement.
