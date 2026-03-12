# From Cloud to Screen: How p3a Connects Pixel Art Communities to a Physical Display

Most digital art lives and dies inside rectangles of glass — phone screens, monitors, tablets. You scroll past it, maybe double-tap a heart, and move on. p3a exists because we wanted pixel art to escape the scroll. To sit on a desk and demand attention. To change and surprise you throughout the day.

But a standalone photo frame gets stale. The magic happens when the frame is connected — when new art can arrive from anywhere, at any time, without you lifting a finger.

## Three Pipelines Into One Frame

p3a draws from three distinct artwork sources, each with its own personality:

### Your Personal Collection

The simplest path. Drop WebP, GIF, PNG, or JPEG files onto a microSD card, slide it into the device, and they enter the rotation. This is for the art you've chosen deliberately — commissions, your own work, favorites downloaded from artists you follow.

Files play in alphabetical order by default, and auto-advance shuffles through them every 30 seconds. The device handles any aspect ratio, scaling artwork up (or down) to fit the 720x720 display while preserving proportions. Pixel art gets nearest-neighbor upscaling to keep those edges razor sharp.

Transparent images work too. If your sprite has a transparent background, p3a composites it over a configurable background color. Set it to black for that classic arcade aesthetic, or match it to your desk setup.

### Giphy: The Internet's Pulse

Plug in a free Giphy API key and p3a taps into the global trending feed. Every few hours (you choose the interval), it refreshes its list of what the internet is watching. The device downloads GIFs on demand as they come up in the rotation — the first play of a new GIF might take a moment to fetch, but after that it's cached on the SD card and plays instantly.

You have full control over content filtering:

- **Content rating** — G through R, so you can safely put this on a shared desk
- **Rendition** — fixed height, fixed width, original, or downsized
- **Format** — WebP (smaller, recommended) or GIF
- **Cache size** — how many GIFs to keep per channel before old ones are evicted

Giphy content is inherently ephemeral. Trends change daily. Your p3a reflects that — what's playing today won't be what's playing next week. It keeps the device feeling alive in a way that a static collection never can.

### Makapix Club: Art From the Community

This is the most personal pipeline. [Makapix Club](https://makapix.club/) is a pixel art social network where artists share their work. Register your p3a (long-press the screen to get a pairing code, enter it on the website), and a secure MQTT channel opens between your device and the cloud.

Now anyone — friends, family, artists you follow — can browse artwork on Makapix and send it directly to your p3a. It arrives instantly. One moment your frame is showing a cycling landscape animation; the next, a friend has pushed a pixel art cat they just finished. The device stores received artwork in a local vault (SHA256-sharded on the SD card) so it enters the permanent rotation.

The connection uses TLS 1.2+ with mutual certificate authentication. Each device gets unique client certificates during registration. Your p3a talks to the cloud, but nobody else can talk to your p3a.

From the Makapix website you can also control the device remotely — skip artwork, adjust brightness, pause playback. Useful when the p3a is across the room, or across the country.

## The Play Scheduler: Mixing It All Together

With three sources feeding artwork into your device, the question becomes: how do you balance them?

p3a's Play Scheduler answers this with **playsets** — declarative commands that define which channels to include, their relative exposure weights, and the selection strategy within each channel.

Think of it like a radio station playlist algorithm. You might configure:

- 50% Makapix Club artwork (the art you've been personally sent)
- 30% local files (your curated collection)
- 20% Giphy trending (fresh internet content)

The scheduler is deterministic — it won't play the same artwork twice in a row, it balances exposure across channels as configured, and it respects per-channel selection strategies (sequential, random, or weighted).

You configure playsets through a web-based editor at `http://p3a.local/`. No code, no config files — just a visual interface for building the rotation you want.

## The Network Stack

Behind the scenes, p3a runs a surprisingly full network stack for a device with no operating system:

**Wi-Fi 6** via the ESP32-C6 co-processor. Setup happens through a captive portal — the device creates a `p3a-setup` network, you connect and enter your Wi-Fi credentials, and it handles the rest. WPA2 and WPA3 are both supported.

**HTTP server** hosts the web interface and REST API. Every button in the web UI maps to an API endpoint you can call from scripts, Home Assistant, or anything that speaks HTTP.

**WebSocket** provides real-time communication for the PICO-8 game streaming mode and live status updates.

**mDNS** advertises the device as `p3a.local` on your network. No IP address hunting required.

**MQTT over TLS** connects to Makapix Club for receiving artwork and remote commands. The connection is persistent and reconnects automatically.

**OTA updates** check GitHub Releases every two hours. When an update is available, you get a notification in the web UI. One click to install, automatic rollback if the new firmware fails to boot. The ESP32-C6 co-processor firmware updates automatically too — one fewer thing to think about.

## USB: The Physical Escape Hatch

Sometimes you just want to drag and drop files. p3a's high-speed USB-C port exposes the microSD card as a standard removable drive. Plug it into your computer, copy artwork on or off, eject, done. The device keeps playing while connected — you just can't change the current artwork until you disconnect.

This also works with phones and tablets that support USB OTG. Pull up pixel art on your phone, copy it to the p3a's SD card, and it's in the rotation.

## Living With It

After weeks of daily use, a few things stand out:

**It's a conversation starter.** People notice the display and ask about it. The Giphy trending feed is particularly good at this — "wait, why is that GIF on your desk?" leads to surprisingly fun conversations.

**The Makapix integration creates connection.** There's something delightful about a friend sending pixel art directly to a physical object in your room. It's more tangible than a notification, more persistent than a message. The art sits there on your desk until the rotation moves on.

**Auto-advance keeps it fresh.** You stop actively thinking about what's displayed and start passively noticing it. A new animation catches your eye, you look up from work for a moment, and then go back to what you were doing. It's ambient art in the truest sense.

**The REST API enables weird and wonderful things.** Trigger artwork changes from CI/CD pipelines (green GIF when tests pass, red when they fail). Sync with your calendar (different art for meeting blocks vs. focus time). The API is simple enough that integration takes minutes.

p3a bridges the gap between digital art communities and physical space. Pixel art was born on screens, but it doesn't have to stay there. Sometimes the best display for a 32x32 masterpiece is a dedicated 720x720 frame on your desk, cycling through art from around the world, one pixel-perfect frame at a time.
