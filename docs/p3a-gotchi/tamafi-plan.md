# p3a-gotchi Implementation Plan

A virtual pet that lives inside the p3a pixel art frame, appearing between artwork swaps and reacting to touch. Based on TamaFi's game logic (MIT license) with new art and a p3a-native integration layer.

## Design Vision

The pet is **not** a separate app — it's a resident creature that inhabits the frame. It appears briefly during artwork transitions: a small animated sprite walking across the screen, sitting in a corner, sleeping, or reacting to the viewer. The user can optionally interact (tap to send a cookie), but the pet is mostly autonomous and ambient. Over days and weeks, it evolves, develops a personality, and reflects how the frame is being used.

## What We Take from TamaFi

TamaFi (github.com/cifertech/TamaFi, MIT license) provides ~250 lines of battle-tested game logic that we extract and adapt. We do **not** port TamaFi's rendering, UI screens, button handling, WiFi scanning, NeoPixel effects, or buzzer code.

### Game state (`Pet` struct)

```c
typedef struct {
    int hunger;        // 0-100, decays over time, restored by "feeding" events
    int happiness;     // 0-100, decays without interaction, boosted by touch/views
    int health;        // 0-100, derived from hunger + happiness thresholds

    uint32_t age_minutes;
    uint32_t age_hours;
    uint32_t age_days;
} p3a_gotchi_pet_t;
```

### Personality traits (randomized at birth via TRNG)

```c
typedef struct {
    uint8_t curiosity; // 40-90, influences explore desire
    uint8_t activity;  // 30-90, influences animation speed
    uint8_t stress;    // 20-80, influences rest desire
} p3a_gotchi_traits_t;
```

### Moods (7 states, derived from stats)

| Mood | Condition |
|------|-----------|
| HUNGRY | hunger < 25 |
| SICK | health < 25 |
| EXCITED | happiness > 80 and frame is active (recent swaps) |
| HAPPY | happiness > 60 |
| BORED | no artwork swaps for a long time |
| CURIOUS | new artwork source detected (e.g. Makapix push) |
| CALM | default |

Mood affects idle animation speed: excited = fast, bored/sick = slow.

### Evolution stages (4 stages, time + stat gated)

| Stage | Requirement |
|-------|-------------|
| BABY | age < 20 min, or stats avg < 35 |
| TEEN | age >= 20 min, stats avg > 35 |
| ADULT | age >= 60 min, stats avg > 45 |
| ELDER | age >= 180 min, stats avg > 40 |

### Autonomous decision engine

Every 8-15 seconds (randomized), the pet decides its next activity. Each option gets a desire score influenced by stats, personality traits, mood, and randomness:

- **Idle**: base desire 10 (do nothing, just animate)
- **Explore**: curiosity trait + recent new-artwork events + random(0,20). The pet wanders across the screen.
- **Rest**: (100 - health) + stress/2. The pet curls up and sleeps, recovering health.
- **Play**: (100 - happiness) + activity/2. The pet bounces or does a trick.

The highest desire wins. This produces believable, non-deterministic behavior.

### Rest state machine (3 phases)

1. **REST_ENTER**: Curl-up animation (400ms per frame)
2. **REST_DEEP**: Sleep for 5-15 seconds (randomized). Stats recover midway (+15 health, +10 happiness, -3 hunger).
3. **REST_WAKE**: Uncurl animation (400ms per frame)

### Stat decay (from TamaFi, adapted rates)

| Stat | Decay interval | Rate | Condition |
|------|---------------|------|-----------|
| Hunger | 5 seconds | -2 | Always |
| Happiness | 7 seconds | -1 to -3 | -3 if no recent artwork swaps, -1 otherwise |
| Health | 10 seconds | -1 to -2 | -2 if hunger < 20 or happiness < 20 |

These rates are from TamaFi and tuned for engagement without being demanding. The pet declines gradually but doesn't die overnight.

### Persistence

Save to NVS (via `config_store`) every 30 seconds and on significant events:
- Pet stats (hunger, happiness, health)
- Age (minutes, hours, days)
- Evolution stage
- Personality traits
- Hatched flag

## What We Replace

### WiFi feeding → p3a event feeding

TamaFi feeds the pet by scanning WiFi networks. We replace this with p3a-native events that "nourish" the pet:

| p3a event | Pet effect | Rationale |
|-----------|-----------|-----------|
| Artwork swap (auto or manual) | hunger +2 | The frame is being used — the pet is "fed" by the art flowing through it |
| Makapix artwork received | hunger +5, happiness +5 | Someone sent art from the cloud — exciting! |
| Touch interaction (tap during pet appearance) | happiness +8 | Direct attention from the user |
| Giphy refresh (new trending batch) | hunger +3 | New food arrived |
| Long idle (no swap for > 5 minutes) | happiness -5 | The frame is gathering dust |
| OTA update completed | happiness +15 | The pet celebrates new firmware |

This turns the pet into a reflection of how actively the frame is being used. An active frame with diverse content sources keeps the pet happy and well-fed. A neglected frame makes the pet sad and hungry.

### Full-screen UI → interstitial overlay

TamaFi has 11 screen modes. We have one: the pet appearing during artwork transitions.

**When the pet appears:**
- During the ~0.5-1s gap between artwork swaps (the moment the old artwork fades and the new one loads)
- Probability: not every swap. Maybe 1 in 3-5 swaps, randomized. The pet shouldn't overstay its welcome.
- Duration: 2-5 seconds, enough to see it and optionally tap

**How it renders:**
- The pet sprite is composited into p3a's framebuffer during the transition
- Background: the current p3a background color (already configurable)
- The pet sprite uses a transparency key color (e.g. 0xFF00FF magenta) so it composites cleanly
- Nearest-neighbor upscaling from sprite native size to display size (p3a already does this well)

**Touch interaction during appearance:**
- Tap anywhere on the pet → "cookie" animation (small heart or star particle), happiness +8
- If the user doesn't tap, the pet just does its thing and fades out
- The existing touch router can be extended with a `GOTCHI_VISIBLE` state that intercepts taps before they go to the normal left/right swap handler

### Buttons → touch gestures

TamaFi uses 6 buttons for navigation, feeding, etc. p3a-gotchi needs exactly one gesture: **tap to interact**. Everything else is autonomous.

Optional future additions:
- Double-tap → feed (hunger boost)
- Swipe down on pet → pet goes to sleep (manual rest trigger)

### Stone Golem art → CC0 or custom pixel art

The TamaFi Stone Golem is well-drawn but tonally wrong for p3a (dark, fantasy, aggressive). We need something friendly, colorful, and small enough to look good as an overlay.

**Recommended approach: CC0 Pet Dogs Pack (LuizMelo, itch.io)**

6 dog breeds with 11 animation states each. CC0 license (no restrictions). Sprite sizes ~20-40px, perfect for nearest-neighbor upscaling.

| Pet state | Dog animation |
|-----------|--------------|
| Idle | Idle (10 frames) |
| Happy/Excited | Run (8 frames), Bark (3 frames) |
| Exploring | Walk (8 frames) |
| Hungry | Sitting (1 frame), Licking (4 frames) |
| Sleeping | Sleeping (1 frame), Lying Down (7 frames) |
| Bored | Itching (2 frames), Stretching (10 frames) |

**Breed selection:** Each p3a device gets a random breed at first boot using the ESP32-P4's TRNG. The breed is saved to NVS and never changes — your p3a has "its" dog.

| Breed | Personality bias |
|-------|-----------------|
| Golden Retriever | High happiness baseline |
| Akita | High curiosity |
| Great Dane | Low stress, slow animations |
| Schnauzer | High activity |
| Saint Bernard | High hunger decay (big dog, big appetite) |
| Siberian Husky | High energy, fast animations |

**Sprite format:** Convert PNG sprite sheets to RGB565 or RGB888 C arrays at build time, or store as PNG on SD card and decode on demand (p3a already has PNG decoding). Transparency via designated key color or alpha channel.

**Memory:** At ~40px wide, 4 bytes/pixel (RGBA), 10 frames: ~6.4 KB per animation. Total for all 11 animations of one breed: ~70 KB. Negligible compared to TamaFi's 862 KB.

## Component Architecture

New ESP-IDF component: `components/p3a_gotchi/`

```
components/p3a_gotchi/
├── CMakeLists.txt
├── Kconfig                          # Enable/disable, appearance frequency, etc.
├── include/
│   └── p3a_gotchi.h                 # Public API
├── p3a_gotchi.c                     # Init, event hooks, tick scheduling
├── p3a_gotchi_logic.c               # Game state machine (extracted from TamaFi)
├── p3a_gotchi_render.c              # Sprite compositing into framebuffer
└── p3a_gotchi_sprites.h             # Sprite data (or SD card loader)
```

### Public API

```c
// Lifecycle
esp_err_t p3a_gotchi_init(void);          // Load state from NVS, pick breed if first boot

// Event hooks (called by other p3a components)
void p3a_gotchi_on_artwork_swap(void);     // Artwork changed (auto or manual)
void p3a_gotchi_on_makapix_receive(void);  // Artwork received from Makapix
void p3a_gotchi_on_giphy_refresh(void);    // New Giphy batch fetched
void p3a_gotchi_on_ota_complete(void);     // Firmware updated
void p3a_gotchi_on_touch(int x, int y);    // Touch during pet visibility

// Rendering
bool p3a_gotchi_should_appear(void);       // Probabilistic: should pet show this swap?
void p3a_gotchi_render(uint8_t *framebuffer, int fb_width, int fb_height, int stride);
void p3a_gotchi_tick(void);                // Called every 100ms from main loop
```

### Kconfig options

```
config P3A_GOTCHI_ENABLE
    bool "Enable p3a-gotchi virtual pet"
    default n

config P3A_GOTCHI_APPEARANCE_PROBABILITY
    int "Probability of pet appearing per swap (1-100%)"
    default 30
    range 1 100

config P3A_GOTCHI_APPEARANCE_DURATION_MS
    int "How long the pet stays visible (ms)"
    default 3000
    range 1000 10000

config P3A_GOTCHI_STAT_SAVE_INTERVAL_MS
    int "Auto-save interval (ms)"
    default 30000
```

### Integration points with existing p3a code

| File | Change | Purpose |
|------|--------|---------|
| `main/CMakeLists.txt` | Add `p3a_gotchi` to REQUIRES | Build dependency |
| `main/playback_controller.c` | Call `p3a_gotchi_on_artwork_swap()` after each swap | Feed event |
| `main/playback_controller.c` | Check `p3a_gotchi_should_appear()` during swap transition | Trigger rendering |
| `main/display_renderer.c` | Call `p3a_gotchi_render()` to composite pet into framebuffer | Visual output |
| `main/app_touch.c` | Route taps to `p3a_gotchi_on_touch()` when pet is visible | Touch interaction |
| `components/makapix/` | Call `p3a_gotchi_on_makapix_receive()` on artwork push | Feed event |
| `components/giphy/` | Call `p3a_gotchi_on_giphy_refresh()` on batch refresh | Feed event |
| `components/ota_manager/` | Call `p3a_gotchi_on_ota_complete()` after successful update | Feed event |
| `main/p3a_main.c` | Call `p3a_gotchi_init()` during boot sequence | Initialization |

### Rendering approach

The pet renders into the existing framebuffer during swap transitions. No separate framebuffer or render pipeline needed.

```
Normal flow:    [Artwork A] → [Artwork B]
With gotchi:    [Artwork A] → [Background + Pet sprite (2-5s)] → [Artwork B]
```

The pet sprite is composited with transparency into the framebuffer that `display_renderer.c` already manages. The pet's position on screen can vary (walking across, sitting in corner, centered) depending on its current activity.

Sprite upscaling: reuse p3a's existing CPU nearest-neighbor upscaler. A 40px sprite at 6x scale = 240px on the 720px display — a comfortable size that doesn't dominate but is clearly visible.

## Implementation Phases

### Phase 1: Game logic extraction
- Create `components/p3a_gotchi/` skeleton
- Port TamaFi's `Pet` struct, `logicTick()`, `updateMood()`, `decideNextActivity()`, `updateEvolution()`, `stepRest()` to plain C
- Replace Arduino types (`String`, `unsigned long`) with C/ESP-IDF equivalents
- Replace `Preferences` with `config_store` for NVS persistence
- Replace WiFi scan feeding with p3a event hooks (stub implementations)
- Unit-testable: the logic should compile and run in isolation with no display or hardware dependency

### Phase 2: Sprite pipeline
- Obtain CC0 dog sprites (LuizMelo Pet Dogs Pack)
- Convert PNG sprite sheets → individual frame PNGs → C arrays or SD card assets
- Implement `p3a_gotchi_render()`: composite a single sprite frame onto an RGB888 framebuffer with transparency
- Test with a static sprite rendered during a swap transition

### Phase 3: Integration
- Hook `p3a_gotchi_on_artwork_swap()` into `playback_controller.c`
- Implement `p3a_gotchi_should_appear()` with configurable probability
- Add `GOTCHI_VISIBLE` state to touch router for tap interception
- Add the `p3a_gotchi_tick()` call to the main loop (or a dedicated low-priority FreeRTOS task)
- Wire up Makapix/Giphy/OTA event hooks

### Phase 4: Polish
- Tune stat decay rates through playtesting
- Add breed-specific personality biases
- Implement "cookie" tap animation (heart particle or similar)
- Add pet status to web UI (optional: a small status card on the dashboard showing pet mood/stats)
- Add Kconfig options for all tunables
- Consider: pet reacts to screen rotation (stumbles), brightness changes (squints), etc.

## Open Questions

1. **Death mechanic**: TamaFi's pet dies when all stats reach 0. For p3a-gotchi, death seems harsh for a background feature. Options: (a) pet cannot die, just gets very sad, (b) pet "runs away" and a new egg appears after a cooldown, (c) pet dies and the user must tap to restart. Leaning toward (b).

2. **Persistence across firmware updates**: NVS survives OTA. The pet should survive firmware updates. Verify that `config_store` keys don't get wiped during OTA.

3. **First-time experience**: On a fresh p3a with no pet data in NVS, should the pet hatch immediately, or should there be a deliberate "hatch" trigger? TamaFi has an egg-hatching animation triggered by button press. For p3a, maybe the egg appears during the first few swap transitions, and a tap hatches it.

4. **Web UI integration**: Should the pet's stats be visible in the web dashboard? A small card showing breed, mood, age, and stats would be charming and low-effort (just a new REST endpoint + a card in `index.html`).

5. **Multiple pets**: Out of scope for v1. But the architecture should not hard-code singleton assumptions, in case we want a second pet or pet breeding later.

6. **Art style consistency**: The CC0 dog sprites have a specific pixel art style. They should look at home alongside the artwork that p3a displays. Evaluate whether the dog sprites' color palette and detail level feel right on the 720x720 display at 6x upscale.
