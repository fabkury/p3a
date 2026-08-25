# Hackaday Tip Email

*Companion to `outreach-plan.md` (maker track). Drafted 2026-06-23.*

Tier 1 outreach to Hackaday's tipline (`tips@hackaday.com`). Hackaday's
[submission guide](https://hackaday.com/2021/04/27/how-best-to-get-your-project-on-hackaday/)
asks for: a clear descriptive title, lots of media, a complete but
concise description, and an obvious "what's the hack?" Editors skim
tips fast, so this draft is short and front-loads the hack. Resubmit
later (with a different angle) if ignored — they admit tipline coverage
is hit-or-miss.

**Framing.** This is the *maker/firmware* venue, distinct from the
museum/IIIF editorial pitches. The hack is the **firmware**, not the
board — an off-the-shelf dev board running an unusually finished
open-source firmware. Lead with the embedded-IIIF-client novelty +
dual-radio architecture + on-device decode, and treat "$40 board, no
soldering, web-flashable" as the low-friction trial story. Pre-empt the
"it's just a dev board with an app" reflex by making the firmware
engineering the headline.

**Status (video-less).** No `{{video_url}}` — the demo video is
deferred, and media is well covered without it: the live **Makezine
article**, the repo's photos, and the animated GIF in the README. The
only optional placeholder is `{{museumpros_thread_url}}` (a link to the
r/MuseumPros thread, if you want to back the social-proof numbers with
a link — drop it or fill it).

---

## Suggested subject

**p3a: a $40 ESP32-P4 art frame with a native IIIF museum client in firmware**

Descriptive, names the chip (ESP32-P4 is undercovered and a keyword
their audience watches), and puts the unusual hack — "IIIF museum
client in firmware" — right in the subject. Alternative if you'd rather
lead with the trial story: *"Web-flashable ESP32-P4 art frame plays
museum collections, GIFs, and pixel art (open source)."*

## Body

> Hi Hackaday,
>
> I've been building **p3a**, open-source firmware that turns an
> off-the-shelf Waveshare ESP32-P4 board — 4-inch 720×720 24-bit IPS
> touchscreen, about $40 — into a self-contained desktop art frame.
> No PCB, no soldering: you flash it from the browser and it runs.
>
> The part that I think fits your "what's the hack?" test is the
> firmware, not the board:
>
> - **A native IIIF client running on a microcontroller.** The ESP32-P4
>   speaks the IIIF Image API straight from firmware and pulls artwork
>   from seven museums' public APIs — the Art Institute of Chicago, the
>   Rijksmuseum, the V&A, the Wellcome Collection, the Statens Museum
>   for Kunst, the Harvard Art Museums, and the Smithsonian — with
>   per-museum rate-limit handling and an on-device Linked Art walk for
>   the Rijksmuseum. As far as I can tell it's the first firmware-level
>   IIIF consumer.
> - **Dual-radio architecture.** The ESP32-P4 has no native Wi-Fi, so
>   an onboard ESP32-C6 runs as a Wi-Fi 6 co-processor over the hosted
>   interface.
> - **On-device decode** of WebP, GIF, PNG/APNG, JPEG, and BMP (both
>   animated and static), using the P4's hardware JPEG codec for the
>   720×720 museum images.
> - **Finished-product touches** that are uncommon for a hobby build: a
>   browser-based web flasher (connect, click, done — no toolchain), OTA
>   updates from GitHub Releases, a GT911 touch UI, and a full REST API
>   plus WebSocket. It's built to run 24/7 on a shelf.
>
> It also cycles trending GIFs from Giphy and animated pixel art from
> Makapix Club, a pixel-art social network I built — you can push
> artworks to the device from anywhere.
>
> It's Apache 2.0 and the work of a single maker. Make: ran a project
> writeup recently, and the launch posts landed well with the relevant
> crowds — 200+ upvotes on r/MuseumPros and 100+ on r/esp32.
>
> Links:
> - Repo, photos, and a demo GIF: https://github.com/fabkury/p3a
> - Web flasher (works in-browser): https://fabkury.github.io/p3a/web-flasher/
> - Make: writeup: https://makezine.com/projects/desktop-pixel-art-player-p3a/
>
> Happy to send higher-res photos or answer anything. Thanks for taking
> a look.
>
> — Fabrício Kury
>   pub@kury.dev

---

## Notes on the choices

- **Hack-first, board-second.** Hackaday's reflex on "off-the-shelf
  board + app" is "where's the hack?" The body answers it immediately:
  the embedded IIIF client, the dual-radio setup, and on-device decode
  are real firmware engineering. The board being cheap and
  solderless is the *trial* story, not the hack.
- **The Make: writeup differentiates rather than competes.** A Make
  project writeup covers the "build one yourself" consumer angle; the
  firmware-as-IIIF-client / dual-radio / hardware-JPEG angle is the
  technical story Hackaday's audience actually wants. Citing it is a
  credibility signal, not "this is leftovers" — Hackaday routinely
  covers projects other maker outlets have touched. Keep it to one
  line, near the end, so it reads as proof not as the lede.
- **Social-proof numbers are appropriate here** (editorial/maker
  venue, per the tiered policy in `museum-outreach-plan.md`). r/esp32 is
  the on-audience number for Hackaday; r/MuseumPros shows cross-domain
  reach. Refresh both on send day. The `{{museumpros_thread_url}}` link
  is optional — Hackaday will mostly judge on the repo and the flasher.
- **"As far as I can tell" on the IIIF-first claim** — never assert a
  flat "first"; let the qualifier stand so a correction (if any) is
  friendly, not a gotcha.
- **Web flasher gets its own bullet and a link.** "Try it in your
  browser right now" is the single lowest-friction thing in the pitch
  and the kind of detail an editor clicks.
- **First-person, no marketing voice.** Tips from the maker, written
  plainly, outperform press-release tone on this tipline.

## Send checklist

- [ ] Refresh the r/esp32 / r/MuseumPros numbers (and fill or remove
      `{{museumpros_thread_url}}`).
- [ ] Confirm the Make:, repo, and web-flasher links resolve.
- [ ] Make sure the repo's hero photos + demo GIF are current — this
      tip leans on them in lieu of a video.
- [ ] Send to `tips@hackaday.com` from a real personal address (not a
      no-reply); a plain-text email reads better than HTML here.
- [ ] If no coverage in ~2–3 weeks, resubmit later with a fresh angle
      (e.g. a single deep feature — "writing an IIIF client for the
      ESP32-P4") rather than re-sending the same tip.
- [ ] Once a demo video exists, add it as a one-line link near the top
      — it strengthens this tip but isn't required to send.
