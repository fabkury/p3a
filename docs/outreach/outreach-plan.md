# p3a Outreach Plan

*Research and recommendations for spreading the word about p3a on a hobby-project budget ($0–$200). Compiled 2026-05-08.*

---

## What you're working with (assets you already have)

You're not starting cold. The strongest hooks for any pitch:

- **Web flasher** — most hobby ESP32 projects lack this. "Click button, get firmware" is the lowest-friction trial story in DIY hardware. Lead with this everywhere.
- **Off-the-shelf board** — no PCB design, no soldering. Anyone with $48 + a screwdriver can build it. Makes "I built one!" posts trivial to generate.
- **Real animated visuals** — pixel art on a 720×720 IPS is photogenic. Demo GIFs already exist. This matters more than copy.
- **ESP32-P4** — newer chip with less coverage than classic ESP32. Hackaday's 2025 hardware retrospective flagged ESP32 family as a perennial favorite, but P4 specifically is undercovered.
- **Makezine credit already in the bag** — that's earned-media credibility you can reference in every subsequent pitch.
- **Makapix Club integration** — the social-network angle is unusual and gives a recurring content story (most pixel art devices are static once shipped).
- **Designed to run 24/7** — both the hardware and the firmware are built for continuous operation. "Set it on a shelf and forget it" is the peace-of-mind story that distinguishes p3a from typical hobby ESP32 builds, and it's worth saying out loud in pitches and on the README.

The natural framing is: **"Tidbyt/Pixoo, but $48, open source, sharper display."** Both Tidbyt ($179) and Divoom Pixoo ($80+) are commercial competitors users actively compare. Adding a Tidbyt/Pixoo comparison table to the README costs nothing and dramatically improves how Reddit/HN evaluate the project.

---

## Tier 1 — One-shot launches (do these RIGHT, not fast)

These are venues where a botched first post leaves a permanent mark. Polish first, then fire.

### 1. Hackaday tip — `tips@hackaday.com`

Hackaday's [submission guide](https://hackaday.com/2021/04/27/how-best-to-get-your-project-on-hackaday/) advises: clear descriptive title, lots of media, complete description, the "what's the hack?" angle obvious. The hack here is "ESP32-P4 + Waveshare board → smart pixel-art frame with social-network backbone." Resubmit if ignored — they admit tipline coverage is hit-or-miss. Expected outcome given project quality: solid odds of a feature.

### 2. Show HN — "Show HN: p3a – Pixel art player firmware for a $40 ESP32-P4 board"

- Post Tue/Wed/Thu 8–10am ET
- Link directly to the GitHub repo
- First comment from author: "what / why / how" mini-essay (not a sales pitch)
- Have README, web flasher, and 30-second demo video tight before posting — HN front-page traffic is brutal on weak landing pages

### 3. Adafruit Show and Tell

Submit via Adafruit's Discord or weekly Wed 7:30pm ET livestream. They post weekly to [their blog](https://blog.adafruit.com/tag/esp32/) — easy to land, very ESP32-friendly, audience overlaps directly with your buyers.

### 4. CNX-Software tip

Jean-Luc Aufranc covers ESP32-P4 launches and obscure hardware. A short email with link + photos has a high hit rate.

---

## Tier 2 — Drip cadence (4–6 weeks, not all at once)

Don't blast everything in one day. Drip across weeks so each post finds a different audience without looking like a coordinated campaign.

| Week | Action |
|------|--------|
| 1 | r/esp32 — "Made an open-source pixel art player on the Waveshare ESP32-P4 board" |
| 2 | r/embedded + r/diyelectronics |
| 3 | r/pixelart — **DM a mod first**; subreddit is strict about self-promo |
| 4 | r/raspberry_pi (alternative-hardware angle) + Hacker News if not yet |
| 5 | [Lospec Discord](https://discord.com/invite/pixelart) — introduce as a maker; offer the device as a way to display members' work |
| ongoing | Bluesky + Mastodon weekly — `#pixelart #ESP32 #ESP32P4 #makers`. [Lospec recommends](https://lospec.com/articles/pixel-art-communities/) Bluesky/Mastodon over X for the pixel-art crowd |

Also low-cost / no-cost:

- **PR to [`agucova/awesome-esp`](https://github.com/agucova/awesome-esp)** — instant evergreen visibility
- **GitHub Topics** — add `esp32-p4`, `pixel-art`, `smart-display`, `open-source-hardware`, `digital-frame` so search surfaces find you
- **Submit to next [Hackaday Prize](https://hackaday.io/contests)** when announced (free)
- **Post build logs** to [Hackaday.io](https://hackaday.io/) and [Hackster.io](https://www.hackster.io/) — these get crawled by news sites and eventually picked up

---

## Tier 3 — Where to spend the $100–$200

### Highest ROI: send 2–3 finished units to YouTubers

A reviewed unit can deliver more reach than a viral Reddit post and lasts longer. Cost: ~$50/unit + shipping × 3 = ~$180. Cold-email pitch with the demo GIF; offer no-strings review unit. Targets in priority order:

1. **[Andreas Spiess](https://www.youtube.com/@AndreasSpiess)** — Swiss accent, ESP32-focused; near-perfect fit
2. **Adafruit Show and Tell** (free — Discord, no shipping needed)
3. **One of**: GreatScott!, Big Mess o' Wires, 8-Bit Show and Tell, Atomic Shrimp, Foone (X/Bluesky)
4. **Hackaday's video desk** if they covered the tip

### Alternative spend: a 30-second sizzle video

If shipping units feels logistically heavy, $50–100 on Fiverr for a 30-second vertical demo video (music + smooth cuts of varied artworks + touch interactions + web flasher demo in 5s). One asset that drops into every pitch, every social post, and the README.

### Don't spend on

- Paid ads (terrible ROI for hobby hardware)
- Product Hunt (mismatched audience)
- Boosted Reddit posts

---

## Skip these

- **Mass-blasting subreddits** — gets you flagged as spam permanently
- **Reposting the same announcement everywhere on the same day** — looks coordinated, harms credibility on HN
- **Discord raids** — joining a server to drop a link is a fast way to get banned and resented
- **Tindie / Crowd Supply** — only if you're going to sell pre-flashed units, not for promoting open-source firmware
- **AI-generated promo images** — pixel-art audiences will notice and roast you

---

## Small README tweaks that compound

Two cheap edits that materially increase conversion before launching:

1. **Comparison table near the top** — p3a vs Tidbyt vs Pixoo on resolution, price, openness, content sources. Reviewers will reach for this.
2. **A "Why I built this" paragraph** — the personal story is what Hackaday / HN actually reward. The current README is excellent but reads as product copy; add 2–3 sentences of human motivation.

---

## Suggested launch sequence (concrete)

1. **Week 0 — Polish**
   - Add Tidbyt/Pixoo comparison table to README
   - Add "Why I built this" paragraph to README
   - Record/commission 30-second sizzle video
   - Build a press kit folder: 3–5 high-res photos (3024px+), logo, one-paragraph and one-page descriptions, the demo GIF
   - Pin a "Feedback / questions welcome" GitHub issue
2. **Week 1**
   - Hackaday tip + Adafruit Show and Tell submission
   - r/esp32 launch post
   - Bluesky + Mastodon launch post
3. **Week 2**
   - Show HN (Tue/Wed/Thu morning)
   - r/embedded + r/diyelectronics
   - CNX-Software tip
4. **Week 3**
   - r/pixelart (mod-approved)
   - Lospec Discord introduction
   - PR to `awesome-esp`
5. **Week 4**
   - r/raspberry_pi (alternative-hardware angle)
   - Cold-email YouTubers with units offered
6. **Ongoing**
   - Weekly Bluesky/Mastodon post (new artwork on display, build photos, user creations)
   - Respond to organic mentions
   - Watch GitHub issues for new-user friction

---

## Open question that affects priority

**Are you OK being publicly visible** — responding to Reddit/HN comments, replying on Bluesky — or do you prefer the project to speak for itself?

Some Tier 1 venues (HN especially) heavily reward author engagement. If you'd rather stay back, the priority order shifts toward Hackaday / Adafruit / CNX, which are more "ship and forget."

---

## Follow-up artifacts to draft

Each takes ~5 minutes and is independently useful. Worth drafting before the launch sequence starts:

- [ ] Hackaday tip email (subject + body)
- [ ] HN title + opening comment
- [ ] README comparison table (p3a vs Tidbyt vs Pixoo)
- [ ] README "Why I built this" paragraph
- [ ] YouTuber cold-email template
- [ ] CNX-Software tip email
- [ ] Reusable social-post copy (Bluesky, Mastodon, Reddit) for the launch wave

---

## Sources

- [Hackaday submit-a-tip](https://hackaday.com/submit-a-tip/)
- [How Best To Get Your Project On Hackaday](https://hackaday.com/2021/04/27/how-best-to-get-your-project-on-hackaday/)
- [Hackaday: 2025 hardware retrospective](https://hackaday.com/2026/01/05/2025-as-the-hardware-world-turns/)
- [Hackster.io](https://www.hackster.io/) — submission platform
- [Hackaday.io contests](https://hackaday.io/contests)
- [Adafruit ESP32 blog tag](https://blog.adafruit.com/tag/esp32/)
- [`agucova/awesome-esp` curated list](https://github.com/agucova/awesome-esp)
- [Lospec — Pixel Art Communities guide](https://lospec.com/articles/pixel-art-communities/)
- [Pixel Joint](https://pixeljoint.com/)
- [Pixel Art Discord (PAD)](https://discord.com/invite/pixelart)
- [Tidbyt Kickstarter](https://www.kickstarter.com/projects/tidbyt/retro-display) — competitor framing
- [Divoom Pixoo 64 product page](https://divoom.com/products/pixoo-64) — competitor framing
- [Waveshare ESP32-P4 + ESP32-C6 PoE board (CNX-Software)](https://www.cnx-software.com/2025/11/19/waveshare-esp32-p4-esp32-c6-poe-development-board-targets-hmi-and-iot-applications/) — example of CNX coverage style
