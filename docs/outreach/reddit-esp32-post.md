# r/esp32 Post

*Drafted 2026-05-17.*

Repost to `r/esp32` (third post about p3a there over ~6 months). The
hook this time is the IIIF museum integration shipped in v0.10.0 —
genuinely new substance since the prior post, which is what makes a
third post acceptable on the subreddit rather than read as
self-promotion churn.

Two versions below: a longer one with more technical bullets for the
"how did you do this" crowd, and a short one that leads with the
museum names and trusts the demo clip to do the rest. The short
version is the current pick; the long one is kept for reference and in
case engagement on the short post calls for an expanded follow-up.

**Submission format.** Reddit video post with the 15-second silent
clip of the device cycling through three museum pieces on a desk as
the primary media; body text below attached to the submission. No
YouTube link — autoplay matters here.

---

## Short version (recommended)

### Title

ESP32-P4 desktop art display now streams from 5 museums via IIIF (Rijksmuseum, Art Institute of Chicago, V&A, Wellcome, SMK)

### Body

> Update from an earlier post: p3a now streams directly from the IIIF
> Image APIs of five museums — Rijksmuseum, Art Institute of Chicago,
> V&A, Wellcome Collection, and SMK.
>
> It's open-source firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
> (720×720 IPS, ESP32-C6 for Wi-Fi 6). Built as a desktop companion —
> small enough to live next to a monitor, sharp enough to show real
> artwork.
>
> IIIF URL construction happens on-device: given an object ID, the
> firmware builds the Image API URL at 720×720 and pulls a JPEG
> directly, no proxy in the middle. Rate-limit cooldown is shared
> between the firmware and the on-device web UI so they don't
> double-spend the museum's quota.
>
> Source + flashing: {{github_url}}. Happy to answer questions.

---

## Long version

### Title

p3a (ESP32-P4 art display) now streams from museum IIIF APIs: Rijksmuseum, Art Institute of Chicago, V&A, Wellcome, SMK

### Body

> Quick context for anyone who wasn't around for earlier posts: p3a is
> open-source firmware for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
> (720×720 IPS, ESP32-C6 for Wi-Fi 6). I built it as a desktop
> companion — small enough to live next to a monitor, sharp enough to
> show real artwork rather than LED-matrix pixel art.
>
> **What's new since last time:** the device now streams directly from
> the IIIF Image APIs of five museums and cycles their collections:
>
> - Rijksmuseum
> - Art Institute of Chicago
> - Victoria & Albert Museum
> - Wellcome Collection
> - Statens Museum for Kunst (SMK)
>
> A few technical bits this sub may find interesting:
>
> - IIIF URL construction happens on-device. Given an object ID, the
>   firmware builds the Image API URL at the right size/quality for
>   the 720×720 panel and pulls a JPEG directly — no proxy server in
>   the middle.
> - Each museum has its own dispatch path (refresh cadence, URL
>   builder, optional Linked-Art resolver for Rijksmuseum since it
>   doesn't expose IIIF images directly through the search API).
> - Rate-limit cooldown is shared between the firmware and the
>   on-device web UI via a small REST endpoint, so browsing the
>   configuration page in your browser doesn't double-spend the
>   museum's quota while the firmware is fetching in the background.
> - Artworks get vaulted to SD under
>   `/sdcard/p3a/museum/{museum_id}/`, so you build up a local
>   collection and don't re-hit the API for things you've already
>   seen.
>
> Rest of the stack, briefly: animated WebP / GIF / PNG / JPEG with
> transparency, MQTT-over-TLS for the Makapix art network, on-demand
> Giphy fetching, triple buffering with VSYNC for the display, OTA
> from GitHub Releases.
>
> Hardware: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (ESP32-P4 + ESP32-C6
> co-processor, 720×720 4" IPS, GT911 touch) — available from
> Waveshare and Amazon.
>
> Source + flashing instructions: {{github_url}}
>
> Happy to answer questions about the IIIF integration, the dual-MCU
> setup, the decoding pipeline, or anything else.

---

## Submission notes

- **Media first.** Upload the 15-sec clip as the post's media (Reddit
  native video, not a YouTube link). The body text is what Reddit
  attaches to a video submission.
- **Silent clip is fine** — most r/esp32 users scroll with sound off.
- **Timing.** Tue–Thu, ~9–11am US Eastern tends to do well on
  r/esp32. Avoid Friday and weekends.
- **First-hour engagement matters.** Be in the comments for the first
  2–3 hours; Reddit's ranking weights early activity heavily, and the
  technical bullets in the long version are designed as bait for
  follow-up questions you can expand on.
- **Pin a first comment with the GitHub link** in case the body gets
  folded on mobile.
- **Don't cross-link** with the IIIF-Discuss or Code4Lib posts —
  audiences overlap modestly and coordinated pushes read as
  marketing.

## Send checklist

- [ ] 15-second demo clip exported and ready to upload.
- [ ] `{{github_url}}` replaced with the canonical repo URL.
- [ ] Posted Tue–Thu morning US time.
- [ ] First reply ready within 30 minutes of submission.
- [ ] Pinned comment with the GitHub link added.
