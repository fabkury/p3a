# p3a-gotchi: Open-Source Tamagotchi Project Landscape

Research compiled March 2026. Evaluates open-source virtual pet projects, C/C++ creature simulations, and pixel art sprite packs for potential integration into p3a as a resident "pet" feature.

## Context

p3a is an ESP32-P4 pixel art player with a 720x720 IPS touch display, dual RISC-V cores @ 360 MHz, 32MB PSRAM, SD card, WiFi 6, and an excellent nearest-neighbor upscaler for pixel art. The goal is to embed a tamagotchi-style virtual pet that lives inside the frame — appearing between artwork swaps, reacting to touch, and evolving over time.

Requirements for a good candidate:
- Permissive license (MIT, Apache, BSD, CC0) — GPL would infect p3a's firmware
- Rich sprite art: many animations, many evolution stages, many frames per animation
- Written in C or C++ (or has extractable game logic)
- Pixel art sprites that upscale well with nearest-neighbor

---

## Part 1: Game Logic Sources (C/C++ Projects)

### Tier 1: Best Candidates for Game Logic

#### TamaFi / TamaFi V2

| | |
|---|---|
| **URL** | https://github.com/cifertech/TamaFi |
| **License** | MIT |
| **Language** | C (98.7%), C++ (1.3%) |
| **Platform** | ESP32 + ST7789 240x240 TFT |
| **Art included** | Yes — custom color sprites in `ui_anim.h` with frame tables |
| **Game mechanics** | Evolution (Baby → Teen → Adult → Elder), hunger/happiness/health, autonomous decision engine with personality traits, 7 moods, rest state machine, WiFi feeding, persistence |

**Suitability for p3a: HIGH — best game logic source found**

The strongest candidate for game logic extraction. Properly separated architecture: game state machine in `TamaFi.ino` (~1168 lines), rendering in `ui.cpp` (~770 lines), shared types in `ui.h`. The rendering uses `TFT_eSprite` (off-screen framebuffer composited to display), which is conceptually close to p3a's triple-buffered approach.

Key game systems worth extracting (~250 lines of pure logic):
- **Autonomous decision engine**: Pet independently chooses to hunt, explore, rest, or idle based on desire scores influenced by stats, personality traits, mood, and randomness. Decisions every 8-15 seconds.
- **Personality traits**: Curiosity, Activity, Stress — randomized per pet via TRNG at birth. Creates unique behavior per device.
- **Mood system**: 7 moods (Hungry, Happy, Curious, Bored, Sick, Excited, Calm) derived from stat combinations. Mood feeds back into animation speed and decision weights.
- **Rest state machine**: 3-phase sleep cycle (enter → deep → wake) with animation sequencing and stat recovery.
- **Stat decay**: Hunger every 5s, happiness every 7s, health every 10s. Rates vary based on context.
- **NVS persistence**: Full save/load of stats, age, evolution stage, personality traits.

**Art**: "Stone Golem" creature — well-drawn pixel art but tonally wrong for p3a (dark, fantasy, aggressive). The art would be replaced entirely. Sprites are 115x110px, RGB565 C arrays with 0xFFFF transparency. Total sprite data: ~862 KB across idle (4f), egg hatch (5f), egg idle (4f), attack (3f), hunger effect (4f), death (3f), and 2 backgrounds.

**Porting effort**: Medium. The game logic ports almost directly to C. The rendering and WiFi feeding mechanic would be replaced entirely. See `tamafi-plan.md` for detailed porting plan.

---

#### DigiCat

| | |
|---|---|
| **URL** | https://github.com/aquascape123/digicat |
| **License** | MIT |
| **Language** | C++ (56%), HTML (38%), C (5%) |
| **Platform** | ESP32 CYD (Cheap Yellow Display) with touch |
| **Art included** | Yes — cat sprites drawn programmatically via TFT_eSPI draw calls |
| **Game mechanics** | 3 evolution stages (Kitten → Young Cat → Adult Cat), hunger/happiness/health, feed/play/pet/sleep, touch interaction |

**Suitability for p3a: MEDIUM — clean but shallow**

The cleanest codebase (single 769-line .ino file) with a straightforward state machine. Touch interaction already built in. However, the game logic is simpler than TamaFi — no personality traits, no autonomous decision engine, no mood system. Only ~80 lines of actual game logic.

Art is code-drawn (procedural `fillCircle`/`fillTriangle` calls), not bitmap sprites. Renders at any resolution but looks geometric and basic. Not rich enough for a 720x720 display.

**Best used as**: Quick reference for stat balancing and touch interaction patterns, not as a primary porting base. TamaFi's game logic is substantially richer.

---

### Tier 2: Non-ESP32 C/C++ Projects

These were found during a broader search beyond the ESP32 ecosystem. The ESP32-P4 is powerful enough to run more sophisticated code, so we evaluated desktop, retro console, and cross-platform projects.

#### Piropa (Game Boy Color)

| | |
|---|---|
| **URL** | https://github.com/zenzoa/piropa |
| **License** | **CC BY-NC-SA** (non-commercial — incompatible) |
| **Language** | C (GBDK-2020) |
| **Art** | Virtual pet frog with 6 unique adult evolution forms, hatching from tadpole |

A virtual pet frog for Game Boy Color. Tadpole grows into 6 unique adult forms based on care. Catching bugs, growing flowers, care interactions. The art is charming GBC-style pixel art. The evolution system (6 distinct forms based on player behavior) is the most branching of any project found.

**Cannot use** due to CC BY-NC-SA license. Valuable as a design reference for branching evolution paths.

#### openc2e (Creatures Engine)

| | |
|---|---|
| **URL** | https://github.com/openc2e/openc2e |
| **License** | **LGPL** (linking restrictions) |
| **Language** | C++ |
| **Art** | Loads original Creatures game sprites (.SPR, .C16, .S16 formats) — no art included |

Open-source engine for the Creatures game series (Creatures 1/2/3, Docking Station). Supports full creature lifecycle with genetics, breeding, neural networks, and evolution. This is the most sophisticated virtual pet engine in existence — but it's massive (SDL + Qt dependencies), LGPL licensed, and includes no art assets.

**Cannot use** directly, but the game design of Creatures (genetics influencing personality, learning from environment) is the aspirational gold standard.

#### VPet-Simulator (Desktop)

| | |
|---|---|
| **URL** | https://github.com/LorisYounger/VPet |
| **License** | Apache 2.0 (code), custom license for character art |
| **Language** | **C# (WPF)** — not C/C++ |
| **Art** | **200+ animation types**, up to 32 types x 4 states x 3 variants = ~384 animation sets |

The richest virtual pet art in any open-source project. Steam Workshop mod support. However, it's C#/WPF (not portable to ESP32) and the character art has a custom license separate from the Apache 2.0 code license.

**Cannot port**, but establishes the benchmark for animation richness. Nothing in the C/C++ world comes close.

#### wayland-vpets (Linux Desktop)

| | |
|---|---|
| **URL** | https://github.com/furudbat/wayland-vpets |
| **License** | Check repo (not confirmed) |
| **Language** | C++ |
| **Art** | Supports Digimon V-Pet sprites, Clippy, Pokemon, custom sprite sheets |

Desktop overlay that loads sprite sheets from external files with hot-reload configuration. The sprite loading and animation system is a useful architectural reference for configurable sprite-based pets.

---

### Tier 3: Emulators (Technically Impressive, Legally Problematic)

#### TamaLib

| | |
|---|---|
| **URL** | https://github.com/jcrona/tamalib |
| **License** | **GPLv2** |
| **Language** | Pure C (100%) |
| **Platform** | Hardware-agnostic via HAL |

The definitive Tamagotchi P1 emulator — clean C, beautiful HAL abstraction, runs on everything from STM32 to web browsers. Technically trivial to port to ESP32-P4. But **GPLv2 would require p3a's entire firmware to be released under GPL**, and running it requires a Tamagotchi ROM dump.

Multiple ESP32 forks exist (ArduinoGotchi, TamagotchiESP32, EggSP32-Tama), all inheriting the GPL constraint.

**Best used as**: Reference for understanding Tamagotchi game mechanics. Do not link into p3a.

---

### Tier 4: Design References (GPL or Non-C/C++)

These cannot be used directly but contain valuable design lessons.

| Project | License | Language | Key Design Lesson |
|---------|---------|----------|-------------------|
| **Flipper Zero Dolphin** | GPLv3 | C | XP tied to device usage; animation manifests for easy content updates; pet enhances primary product, doesn't compete with it |
| **TiMiNoo** | GPL-3.0 / CC BY-NC 4.0 | C++ | "Pet cannot die" design choice; hygiene as a stat; friend visits |
| **ESP32-TamaPetchi** | MIT | C++ | 5 stats instead of 3 (adds energy + cleanliness); web-based display |
| **Raising Hell** | MIT (code) | C++ | Mini-games, pet resurrection, most feature-complete mechanics |
| **Piropa** | CC BY-NC-SA | C | 6 branching evolution forms based on care style |

---

## Part 2: Sprite Art Sources

### The Gap

No single CC0 pack was found that includes explicit **Happy**, **Sad**, and **Eating** animations for a creature. This is the critical gap. Free packs have combat-oriented states (attack, hurt, die) or pet-care states (idle, walk, sleep, groom) but not emotional states. The $1 Cute Slime pack is the cheapest way to get all three.

### Free CC0 Sprite Packs

#### LuizMelo Pet Cats Pack — TOP PICK (CC0, Free)

| | |
|---|---|
| **URL** | https://luizmelo.itch.io/pet-cat-pack |
| **License** | CC0 — no attribution required |
| **Sprite size** | ~20x14 px per cat |
| **Creatures** | 6 cats |
| **Animations** | **12 states per cat** — the most of any free CC0 creature pack |

**Animations per cat (12):** Idle (10f), Walk (8f), Run (8f), Meow (4f), Lying Down (8f), Itch (2f), Sleeping1 (1f), Sleeping2 (1f), Sitting (1f), Licking1 (5f), Licking2 (5f), Stretching (13f)

**Virtual pet state mapping:**

| Pet state | Cat animation | Notes |
|-----------|--------------|-------|
| Idle | Idle (10f) | |
| Happy | Run (8f), Meow (4f) | Meow as "purring" |
| Hungry | Licking (5f), Sitting (1f) | Creative remap — no explicit "eating" |
| Sleeping | Sleeping1/2 (1f each), Lying Down (8f) | 3 sleep variants! |
| Bored | Itching (2f), Stretching (13f) | |
| Walking/Exploring | Walk (8f) | |

The cats pack has one more animation state than the dogs pack and includes Meow (usable as vocalization/happy). Cats also arguably fit a picture frame aesthetic better than dogs.

---

#### LuizMelo Pet Dogs Pack (CC0, Free)

| | |
|---|---|
| **URL** | https://luizmelo.itch.io/pet-dogs-pack |
| **License** | CC0 — no attribution required |
| **Sprite size** | 19x15 to 43x29 px (varies by breed) |
| **Creatures** | 6 breeds (Golden Retriever, Akita, Great Dane, Schnauzer, Saint Bernard, Siberian Husky) |
| **Animations** | **11 states per dog** |

**Animations per dog (11):** Idle (10f), Walk (8f), Run (8f), Bark (3f), Lying Down (7f), Itching (2f), Sleeping (1f), Sitting (1f), Licking1 (4f), Licking2 (4f), Stretching (10f)

Same strengths as the cats pack. The breed diversity (6 breeds with distinct sizes) enables per-device uniqueness via TRNG. The size variation across breeds adds personality — a Great Dane pet feels different from a Schnauzer.

---

#### LuizMelo Monsters Creatures Fantasy (CC0, Free)

| | |
|---|---|
| **URL** | https://luizmelo.itch.io/monsters-creatures-fantasy |
| **License** | CC0 |
| **Sprite size** | Varies (~50-70px) |
| **Creatures** | 4 (Skeleton, Mushroom, Goblin, Flying Eye) |
| **Animations** | 6 states: Idle (4f), Walk/Run (4-8f), Attack (8f), Shield (4f), Take Hit (4f), Death (4f) |

Combat-oriented but the creatures are charming. The Mushroom creature could work as a quirky pet. Less suitable than the cats/dogs for virtual pet mapping since animations are combat-focused (attack, take hit, death) rather than care-focused (eat, sleep, groom).

---

#### Calciumtrice Animated Creatures (CC-BY 3.0, Free)

| | |
|---|---|
| **URL** | https://opengameart.org/content/animated-slime (and related) |
| **License** | CC-BY 3.0 (attribution required, otherwise fully permissive) |
| **Sprite size** | 32x32 px |
| **Creatures** | Slime (4 colors), Snake, Minotaur, Orcs, Skeleton, Rat, Bat, Deer |
| **Animations** | 5 states each: Idle (10f), Gesture (10f), Walk (10f), Attack (10f), Death (10f) |

A consistent collection of 32x32 creatures with exactly 10 frames per animation — unusually high frame counts for free assets. The "Gesture" animation could serve as a "happy" or "interact" state. The Slime is particularly charming as a virtual pet and scales well. CC-BY 3.0 requires attribution but is otherwise fully permissive.

---

#### rvros Animated Pixel Slime (CC0, Free)

| | |
|---|---|
| **URL** | https://rvros.itch.io/pixel-art-animated-slime |
| **License** | CC0 |
| **Sprite size** | 32x25 px |
| **Creatures** | 1 blue slime (recolorable) |
| **Animations** | 5 states: Idle, Move, Attack, Hurt, Die |

Clean, well-animated slime. CC0. Recolorable to create variants. Fewer states than the LuizMelo packs but the animations are smooth. Could work as a simple, abstract pet.

---

#### Other Free CC0/CC-BY Sources

| Pack | License | Sprites | Animations | Notes |
|------|---------|---------|------------|-------|
| Dog Spritesheets (OpenGameArt) | CC0 | Dogs | Idle, Walk, Run, Jump, Fall, Attack | opengameart.org/content/dog-spritesheets |
| LPC Cats and Dogs (OpenGameArt) | CC-BY/OGA-BY | Cats, Dogs | Walk (4 dir), Sleep, Eat | opengameart.org/content/lpc-cats-and-dogs |
| Ninja Adventure Pack | CC0 | 30+ monsters | Idle, Move | pixel-boy.itch.io/ninja-adventure-asset-pack |
| 16x16 DungeonTileset II | CC0 | ~10 enemies | Walk | 0x72.itch.io/dungeontileset-ii |

---

### Paid Sprite Packs (Budget-Friendly)

These are not CC0 but have custom licenses that allow commercial use in games/products. Listed because they fill the "happy/sad/eating" gap that no free pack covers.

#### Cute Slime Spritesheet — $1

| | |
|---|---|
| **URL** | https://whiteslime.itch.io/cute-slime-spritesheet |
| **License** | Custom — commercial use OK when purchased, no redistribution of raw assets |
| **Price** | $1+ (free demo: 5 animations, full: 9 animations) |
| **Creatures** | 17 color variants of a slime |
| **Animations** | **9 states: Idle, Happy, Sad, Eating, Dying, Hit, Attack, Jump, Walking** |

**This is the only pack found with explicit Happy, Sad, AND Eating animations.** The slime aesthetic is abstract and colorful — it would feel at home on a pixel art display without clashing with any particular art style. 17 color variants provide per-device uniqueness. At $1, this is the cheapest way to get purpose-built virtual pet animations.

---

#### ToffeeCraft Pet Mobile Pixel Asset Pack — $1.80

| | |
|---|---|
| **URL** | https://toffeecraft.itch.io/pet-virtual-mobile-pixel-asset |
| **License** | Custom — commercial use OK when purchased, modification OK, no redistribution of raw assets |
| **Price** | $1.80+ |
| **Creatures** | Multiple cats, 7 dog breeds, 10 bunny colors, 5 parrots, 8 birds |
| **Animations** | **12 states**: Idle, Run, Sleep, Eating, Lay Down, Attack, Hurt, Bark, Sitting, Die, Dancing, Surprising |

**Purpose-built for virtual pet games.** The most comprehensive virtual-pet-specific sprite pack found anywhere. Includes furniture, backgrounds, and UI elements alongside the creatures. The "Dancing" and "Surprising" animations are unique to this pack and perfect for reactions to events (new Makapix artwork → surprise animation).

---

#### Catset — $19.99

| | |
|---|---|
| **URL** | https://seethingswarm.itch.io/catset |
| **License** | Custom commercial license |
| **Price** | $19.99 |
| **Sprite size** | 40x40 px (cat ~19x17 px) |
| **Creatures** | 5 cats |
| **Animations** | **23 states** — the most of any pack found anywhere |

23 animation states: Sit, Idle, Idle Blink, Walk, Run, Jump, Fall, Land, Dash, Crouch, Sneak, Attack, Fright, Hurt, Die, Wallgrab, Wallclimb, Liedown, Sleep, Ledgegrab, Ledgeclimb, Ledgeclimb-Struggle, Ledge Idle. Designed for a platformer but many states map to virtual pet use (sit, idle, walk, sleep, fright = scared, hurt = sick).

---

#### Elthen's 2D Pixel Art Cat Sprites — Free (name your price)

| | |
|---|---|
| **URL** | https://elthen.itch.io/2d-pixel-art-cat-sprites |
| **License** | Custom — commercial/non-commercial OK, no redistribution, no blockchain |
| **Sprite size** | 32x32 px |
| **Creatures** | 1 white cat, 4 color variants |
| **Animations** | 9 states: Idle (x2), Clean (x2), Movement (x2), Sleep, Paw, Jump, Scared |

Clean pixel art. The "Scared" and "Clean" animations are useful for virtual pet states. Free but not CC0.

---

### Art Decision Matrix

| | CC0 Cats | CC0 Dogs | Slime ($1) | ToffeeCraft ($1.80) | Catset ($20) |
|---|---|---|---|---|---|
| Price | Free | Free | $1 | $1.80 | $19.99 |
| License | CC0 | CC0 | Custom | Custom | Custom |
| Animations | 12 | 11 | 9 | 12 | 23 |
| Creatures | 6 | 6 | 17 colors | 30+ | 5 |
| Has Idle | Yes | Yes | Yes | Yes | Yes |
| Has Sleep | Yes (3 variants!) | Yes | No | Yes | Yes |
| Has Eat | No (remap Licking) | No (remap Licking) | **Yes** | **Yes** | No |
| Has Happy | No (remap Meow/Run) | No (remap Bark/Run) | **Yes** | No (remap Dancing) | No |
| Has Sad | No | No | **Yes** | No (remap Hurt) | No |
| Has Walk | Yes | Yes | Yes | Yes (Run) | Yes |
| Purpose-built for vpet | Partially | Partially | **Yes** | **Yes** | No |

---

## Part 3: Overall Assessment

### The Landscape Reality

After searching 30+ GitHub repos, itch.io, OpenGameArt, and the broader web:

**There is no single open-source C/C++ project that combines both rich permissive-licensed art AND rich game logic.** The space is fragmented:

- **Best game logic**: TamaFi (MIT, C, ~250 lines of well-tuned state machine with personality traits and autonomous decisions)
- **Best art richness (open-source)**: VPet-Simulator (C#, 200+ animation types — but C#/WPF, not portable)
- **Best art richness (C/C++)**: TamaFi's Stone Golem (115x110px, 23 sprite frames — but tonally wrong for p3a)
- **Best free art for virtual pet**: LuizMelo Pet Cats Pack (CC0, 12 animation states, 6 cats)
- **Best cheap art for virtual pet**: Cute Slime ($1, explicit happy/sad/eating) or ToffeeCraft ($1.80, purpose-built for vpet games)

### Recommended Strategy

**Game logic**: Extract from TamaFi (MIT). The autonomous decision engine with personality traits, mood system, rest state machine, and tuned stat decay rates represent real gameplay iteration that would take days to get right from scratch.

**Art**: Three viable paths depending on budget and licensing preference:

| Path | Art Source | Cost | Pros | Cons |
|------|-----------|------|------|------|
| **A: Free** | LuizMelo Pet Cats (CC0) | $0 | Zero legal risk, 12 states, 6 cats | No explicit happy/sad/eat — requires creative remapping |
| **B: Cheap** | Cute Slime ($1) | $1 | Explicit happy/sad/eating, 17 colors, abstract aesthetic | Custom license (no redistribution of raw assets), fewer total states (9) |
| **C: Comprehensive** | ToffeeCraft Pet Pack ($1.80) | $1.80 | Purpose-built for vpet, 12 states, 30+ creatures with cats/dogs/bunnies/birds | Custom license, not CC0 |

All three paths pair well with TamaFi's game logic. The choice depends on how important the "happy/sad/eating" animation gap is versus zero-cost CC0 licensing.

---

## License Summary

### Game Logic Projects

| Project | Code License | Art License | Safe for p3a? |
|---------|-------------|-------------|---------------|
| TamaFi | MIT | MIT (bundled) | **Yes** |
| DigiCat | MIT | MIT (bundled) | **Yes** |
| ESP32-TamaPetchi | MIT | N/A (web-rendered) | Yes (logic only) |
| Raising Hell | MIT | Separate — verify | Maybe |
| TamaLib | **GPLv2** | N/A | **No** |
| ArduinoGotchi | **GPLv2** | N/A | **No** |
| openc2e | **LGPL** | N/A (no art included) | **No** |
| Piropa | **CC BY-NC-SA** | **CC BY-NC-SA** | **No** |
| Tamaguino | **GPL-3.0** | GPL-3.0 | **No** |
| TiMiNoo | **GPL-3.0** | **CC BY-NC 4.0** | **No** |
| Flipper Dolphin | **GPLv3** | **GPLv3** | **No** |

### Art Packs

| Pack | License | Safe for p3a? | Notes |
|------|---------|---------------|-------|
| LuizMelo Pet Cats | CC0 | **Yes** | No restrictions whatsoever |
| LuizMelo Pet Dogs | CC0 | **Yes** | No restrictions whatsoever |
| LuizMelo Monsters Creatures Fantasy | CC0 | **Yes** | No restrictions whatsoever |
| rvros Animated Pixel Slime | CC0 | **Yes** | No restrictions whatsoever |
| Calciumtrice Animated Creatures | CC-BY 3.0 | **Yes** (with attribution) | Must credit author |
| Cute Slime ($1) | Custom | **Yes** (with purchase) | Cannot redistribute raw assets |
| ToffeeCraft Pet Pack ($1.80) | Custom | **Yes** (with purchase) | Cannot redistribute raw assets |
| Catset ($20) | Custom | **Yes** (with purchase) | Cannot redistribute raw assets |
| Elthen's Cat Sprites | Custom | **Yes** | Cannot redistribute raw assets |
