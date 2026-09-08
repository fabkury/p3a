# Demo clip shot list

Target: one clip, 60-90 seconds, played from disk during the "what p3a
is" section (slide 4, about 1:30 into the talk). Silent, or with a
single ambient track; Fab narrates live over it. Save as `demo.mp4` in
this folder (git-ignored); the fallback is
`images/videos/art-institution-channels/p3a-museums-2026-05-19.mp4`.

This audience wants to see the implementation, not a product ad. Every
shot should answer "is that really a microcontroller talking to our
IIIF server?"

## Setup

- Phone on a tripod, 4K at 30 fps, exposure and white balance locked.
  Plain matte background, indirect light from one side, no glare on the
  panel.
- Device powered from the wall. Wi-Fi confirmed. Museum channels
  already refreshed and cached, so nothing downloads on camera.
- Playset for the shoot: one museum channel per museum that is live
  (Rijks, V&A, Wellcome, SMK, Harvard, Smithsonian, Cleveland,
  Minneapolis), 10 seconds per artwork for the shoot only (default is
  30). Pick channels with strong, square-friendly works: Rijks "Dutch
  Paintings of the Seventeenth Century", V&A "Photographs", Wellcome
  "Paintings" or a genre, SMK "Paintings", Harvard a classification,
  Smithsonian SAAM or NPG, Cleveland "Modern European Painting and
  Sculpture", Mia "Paintings".
- Laptop or phone with the web UI open at `http://p3a.local/playset-editor`
  for the browse-modal shots. Second phone or screen recording for
  those.

## Shots, in order

| # | Length | Shot | Why |
|---|---|---|---|
| 1 | 8 s | Wide establishing shot: the device on a desk or shelf, artwork on screen, something for scale (a mug, a book). | Answers "what is it" before any words. |
| 2 | 20 s | Static close on the panel while it rotates through 3-4 artworks from different museums. Hold each long enough to read. | The core demo: real museum images, sized to the panel. |
| 3 | 10 s | Screen recording: web app, tap the info button on the current artwork; title, artist, date, and museum appear. | Shows attribution and that the browser, not the device, fetches metadata. |
| 4 | 15 s | Screen recording: playset editor, Add Channel, type Museum, pick Rijksmuseum, pick a curated set, preview shows one artwork with Previous/Next, Add channel. | Shows discovery: the museum's own facets, the 193 sets, the lazy Linked Art preview. |
| 5 | 8 s | Back on the device: the new channel's first artwork appears. | Closes the loop from browse to pixels. |
| 6 | 8 s | Optional: the board with its back visible, or the board next to the assembled frame. | "It really is a $40 board." |
| 7 | 6 s | Final hold on one striking artwork. | Clean out-point for the narration to land on. |

Total about 75 seconds. Cut shots 6 and 3 first if it runs long.

## Do not

- No music with lyrics; no captions on the clip (the slides carry the
  words).
- No zooms or handheld moves; every shot static.
- Do not show the AIC channel unless it is unblocked by shoot day.
- Do not show a channel mid-download (progress overlay) unless the point
  is to show it; keep the demo to a stocked cache.

## Narration cues (for rehearsal, spoken live)

- Shot 1-2: "This is p3a. Forty-dollar board, four-inch square panel, no
  operating system, no browser. Everything on the screen right now came
  over the IIIF Image API from the museum's own server."
- Shot 3: "The device stores no metadata. When you tap the info button,
  your phone asks the museum."
- Shot 4: "Channels are the museum's own facets. Here is a Rijksmuseum
  curated set; the preview is resolving a Linked Art chain live."
- Shot 5-7: "And a minute later it is on the frame."
