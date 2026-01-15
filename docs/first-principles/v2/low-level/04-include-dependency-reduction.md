# Include Dependency Reduction

> **Extends**: v1/low-level/deduplicate-includes-and-config.md  
> **Phase**: 4 (Cleanup and Optimization)

## Goal

Reduce the number of includes in key files, eliminate circular dependencies, and make the dependency graph clear and maintainable.

## Current State (v2 Assessment)

### animation_player.c: 24 Includes

```c
#include "animation_player_priv.h"
#include "sd_path.h"
#include "play_scheduler.h"
#include "sdcard_channel_impl.h"
#include "playlist_manager.h"
#include "download_manager.h"
#include "ugfx_ui.h"
#include "config_store.h"
#include "ota_manager.h"
#include "sdio_bus.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "swap_future.h"
#include "p3a_state.h"
#include "makapix_channel_impl.h"
#include "makapix.h"
#include "p3a_render.h"
#include "display_renderer.h"
#include "makapix_channel_events.h"
#include "fresh_boot.h"
#include "connectivity_state.h"
// Plus standard library and ESP-IDF headers
```

**Problem**: A single file depends on nearly every component.

### p3a_main.c: Duplicate Include

```c
#include "freertos/task.h"
// ... many lines later ...
#include "freertos/task.h"  // Duplicate
```

### play_scheduler.c: Complex Dependencies

```c
#include "play_scheduler.h"
#include "play_scheduler_internal.h"
#include "channel_interface.h"
#include "channel_cache.h"
#include "load_tracker.h"
#include "animation_swap_request.h"
#include "sdcard_channel_impl.h"
#include "makapix_channel_impl.h"
#include "makapix_channel_utils.h"
#include "view_tracker.h"
#include "config_store.h"
#include "connectivity_state.h"
#include "p3a_state.h"
#include "sd_path.h"
// ... more
```

## v1 Alignment

v1's "Deduplicate Includes and Config" identifies duplicate includes. v2 provides a comprehensive reduction strategy.

## Dependency Categories

### Category A: Legitimate Dependencies

Headers that are genuinely needed for the file's functionality:
- Type definitions used in function signatures
- Macros used in the implementation
- Constants referenced directly

### Category B: Forward-Declarable

Types that could be forward-declared instead of included:
- Pointers to opaque types
- Function pointer types
- Enum types used only in casts

### Category C: Indirect Dependencies

Headers included because another header needs them:
- Should be fixed in the source header
- Use include-what-you-use principle

### Category D: Dead Code Dependencies

Headers for features that are unused or conditionally compiled:
- Debug-only includes
- Disabled feature includes

## Reduction Strategies

### Strategy 1: Forward Declarations

```c
// Before: full include
#include "makapix.h"  // Just for makapix_state_t

// After: forward declaration
typedef enum makapix_state_e makapix_state_t;
makapix_state_t makapix_get_state(void);
```

### Strategy 2: Opaque Pointers

```c
// Before: include for type definition
#include "channel_interface.h"
void do_something(channel_handle_t ch);

// After: forward declare handle
typedef struct channel_s* channel_handle_t;
void do_something(channel_handle_t ch);
```

### Strategy 3: Move Implementation Details

```c
// Before: public header includes internal
// animation_player.h
#include "sdcard_channel.h"  // For asset_type_t

// After: define needed types locally or re-export
typedef enum { ASSET_WEBP, ASSET_GIF, ASSET_PNG, ASSET_JPEG } asset_type_t;
```

### Strategy 4: Private Headers

Split public/private:

```c
// animation_player.h - public, minimal includes
// animation_player_priv.h - private, has implementation includes

// In animation_player.c:
#include "animation_player_priv.h"  // Gets everything needed internally
```

### Strategy 5: Conditional Includes

```c
// Only include when feature enabled
#if CONFIG_P3A_PICO8_ENABLE
#include "pico8_stream.h"
#include "pico8_render.h"
#endif
```

## Specific Reduction Targets

### animation_player.c

| Current Include | Action | Reason |
|-----------------|--------|--------|
| `sdcard_channel_impl.h` | Remove | Use `channel_interface.h` |
| `makapix_channel_impl.h` | Remove | Use factory function |
| `makapix_channel_events.h` | Remove | Use event bus |
| `download_manager.h` | Remove | Not directly used |
| `playlist_manager.h` | Remove | Indirect via play_scheduler |
| `ugfx_ui.h` | Conditional | Only for UI mode |
| `ota_manager.h` | Remove | Not used in this file |
| `fresh_boot.h` | Conditional | Debug only |

**Target**: Reduce from 24 to ~10 includes.

### p3a_main.c

| Current Include | Action | Reason |
|-----------------|--------|--------|
| Duplicate `freertos/task.h` | Remove | Duplicate |
| Multiple state headers | Consolidate | Use unified state |

**Target**: Remove duplicates, consolidate state includes.

### play_scheduler.c

| Current Include | Action | Reason |
|-----------------|--------|--------|
| `sdcard_channel_impl.h` | Remove | Use interface |
| `makapix_channel_impl.h` | Remove | Use interface |
| `view_tracker.h` | Move to makapix | Domain separation |

**Target**: Use abstractions instead of implementations.

## Implementation Steps

### Step 1: Create Include Graph

```bash
# Generate include graph for analysis
python scripts/include_graph.py main/animation_player.c
```

Or use manual analysis:

```bash
rg "^#include" main/animation_player.c | wc -l
```

### Step 2: Identify Removable Includes

For each include, ask:
1. Is it actually used?
2. Can it be forward-declared?
3. Should it be in a private header?

### Step 3: Create Private Headers

```c
// animation_player_priv.h
#ifndef ANIMATION_PLAYER_PRIV_H
#define ANIMATION_PLAYER_PRIV_H

// Internal includes grouped here
#include "sdcard_channel_impl.h"
#include "makapix_channel_impl.h"
// ... etc

// Internal types and state
typedef struct { ... } animation_player_state_t;

#endif
```

### Step 4: Update Public Headers

Remove internal includes from public headers:

```c
// animation_player.h - PUBLIC
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Forward declarations instead of includes
typedef struct channel_s* channel_handle_t;
typedef enum display_rotation_e display_rotation_t;
```

### Step 5: Fix Circular Dependencies

If A includes B and B includes A:
1. Identify which direction is primary
2. Use forward declarations in the other
3. Or extract shared types to a third header

### Step 6: Add Include Guards

Ensure all headers have proper guards:

```c
#ifndef COMPONENT_MODULE_H
#define COMPONENT_MODULE_H

// ... content

#endif // COMPONENT_MODULE_H
```

## Verification

### Compile Test

```bash
idf.py build 2>&1 | grep "undefined reference"
idf.py build 2>&1 | grep "unknown type"
```

### Include-What-You-Use

Consider running IWYU tool:

```bash
# If available
iwyu_tool.py -p build main/animation_player.c
```

## Success Criteria

- [ ] No file has > 15 includes
- [ ] No duplicate includes
- [ ] No circular include dependencies
- [ ] Public headers have minimal includes
- [ ] Private implementation details in `*_priv.h`

## Risks

| Risk | Mitigation |
|------|------------|
| Missing type definitions | Compile and test after each change |
| Breaking other files | Incremental changes, one file at a time |
| Forward declaration errors | Ensure types match exactly |
