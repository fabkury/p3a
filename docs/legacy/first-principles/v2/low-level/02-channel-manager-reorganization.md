# Channel Manager Reorganization

> **New in v2**  
> **Phase**: 4 (Cleanup and Optimization)

## Goal

Consolidate the 15+ header files in `channel_manager/include/` into logical groups, reducing cognitive load and making the component easier to navigate.

## Current State (v2 Assessment)

`components/channel_manager/include/` contains:

```
animation_metadata.h
animation_swap_request.h
channel_cache.h
channel_interface.h
channel_settings.h
download_manager.h
load_tracker.h
makapix_channel_events.h
makapix_channel_impl.h
makapix_channel_utils.h
pcg32_reversible.h
playlist_manager.h
sdcard_channel_impl.h
sdcard_channel.h
vault_storage.h
```

**15 headers** for one component is excessive and indicates unclear boundaries.

## Analysis by Purpose

| Header | Purpose | Should Be |
|--------|---------|-----------|
| `channel_interface.h` | Core vtable abstraction | **Public** |
| `sdcard_channel.h` | SD channel types | Public |
| `sdcard_channel_impl.h` | SD implementation | **Internal** |
| `makapix_channel_impl.h` | Makapix implementation | **Internal** |
| `makapix_channel_events.h` | Event definitions | Internal |
| `makapix_channel_utils.h` | Makapix helpers | Internal |
| `vault_storage.h` | Cache storage | Internal or separate component |
| `download_manager.h` | Network downloads | Internal or separate component |
| `playlist_manager.h` | Playlist handling | Internal |
| `channel_cache.h` | LAi caching | Internal |
| `channel_settings.h` | Per-channel settings | Internal |
| `animation_metadata.h` | Sidecar metadata | Could be separate |
| `animation_swap_request.h` | Shared type | Move to play_scheduler |
| `load_tracker.h` | Load failure tracking | Internal |
| `pcg32_reversible.h` | PRNG algorithm | Utility, not channel-specific |

## Proposed Reorganization

### Option A: Subdirectory Grouping

```
channel_manager/
├── include/
│   ├── channel_interface.h      # Public: core abstraction
│   ├── sdcard_channel.h         # Public: SD types and factory
│   └── makapix_channel.h        # Public: Makapix types and factory (new)
│
├── internal/
│   ├── sdcard_channel_impl.h
│   ├── makapix_channel_impl.h
│   ├── makapix_channel_events.h
│   ├── makapix_channel_utils.h
│   ├── playlist_manager.h
│   ├── channel_cache.h
│   ├── channel_settings.h
│   └── load_tracker.h
│
├── CMakeLists.txt
└── *.c
```

### Option B: Component Split

Split into multiple components:

```
components/
├── channel_interface/           # Core abstraction
│   ├── include/
│   │   └── channel_interface.h
│   └── channel_interface.c      # Shared utilities
│
├── sdcard_channel/              # SD card implementation
│   ├── include/
│   │   └── sdcard_channel.h
│   └── sdcard_channel.c
│
├── makapix_channel/             # Makapix implementation
│   ├── include/
│   │   └── makapix_channel.h
│   └── makapix_channel.c
│
├── content_cache/               # Vault + download (renamed)
│   ├── include/
│   │   └── content_cache.h
│   └── content_cache.c
│
└── animation_metadata/          # Sidecar parsing
    ├── include/
    │   └── animation_metadata.h
    └── animation_metadata.c
```

## Recommended Approach: Option A (Subdirectory)

Option A is less disruptive and achieves the goal of reducing public API surface.

### Migration Steps

#### Step 1: Create Internal Directory

```bash
mkdir components/channel_manager/internal
```

#### Step 2: Move Internal Headers

```bash
mv include/sdcard_channel_impl.h internal/
mv include/makapix_channel_impl.h internal/
mv include/makapix_channel_events.h internal/
mv include/makapix_channel_utils.h internal/
mv include/playlist_manager.h internal/
mv include/channel_cache.h internal/
mv include/channel_settings.h internal/
mv include/load_tracker.h internal/
```

#### Step 3: Update CMakeLists.txt

```cmake
idf_component_register(
    SRCS ...
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "internal"  # Add private includes
    ...
)
```

#### Step 4: Update Source Files

```c
// Before
#include "makapix_channel_impl.h"

// After
#include "../internal/makapix_channel_impl.h"
// Or simply rely on PRIV_INCLUDE_DIRS
```

#### Step 5: Consolidate Makapix Headers

Create single public `makapix_channel.h`:

```c
// include/makapix_channel.h
#ifndef MAKAPIX_CHANNEL_H
#define MAKAPIX_CHANNEL_H

#include "channel_interface.h"

// Factory function
channel_handle_t makapix_channel_create(const char* channel_type, 
                                         const char* identifier);

// Channel types
#define MAKAPIX_CHANNEL_ALL "all"
#define MAKAPIX_CHANNEL_PROMOTED "promoted"
#define MAKAPIX_CHANNEL_USER "user"
#define MAKAPIX_CHANNEL_BY_USER "by_user"
#define MAKAPIX_CHANNEL_HASHTAG "hashtag"

#endif
```

#### Step 6: Move Shared Types

`animation_swap_request.h` defines `swap_request_t` used by `play_scheduler`. Move to:

```
components/play_scheduler/include/animation_swap_request.h
```

Or create a shared types component:

```
components/p3a_types/include/animation_swap_request.h
```

#### Step 7: Relocate PRNG

`pcg32_reversible.h` is a general utility. Options:
- Move to `components/utils/`
- Move to `components/play_scheduler/` (primary user)
- Keep as-is if only used internally

## Final Public API

After reorganization, external consumers see only:

```c
#include "channel_interface.h"   // Core abstraction
#include "sdcard_channel.h"      // SD channel factory
#include "makapix_channel.h"     // Makapix channel factory
```

**3 public headers** instead of 15.

## Success Criteria

- [ ] Public `include/` has ≤5 headers
- [ ] Internal headers in `internal/` or `priv/`
- [ ] No external component includes internal headers
- [ ] `animation_swap_request.h` moved to appropriate owner
- [ ] Build succeeds with `-Werror` for missing includes

## Risks

| Risk | Mitigation |
|------|------------|
| Breaking external includes | Search codebase for all includers |
| CMake path issues | Test build after each step |
| Circular dependencies | Audit include graph first |
