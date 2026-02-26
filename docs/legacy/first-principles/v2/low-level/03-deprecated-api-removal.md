# Deprecated API Removal

> **New in v2**  
> **Phase**: 4 (Cleanup and Optimization)

## Goal

Remove deprecated functions and APIs that have been superseded by newer implementations, reducing code maintenance burden and potential confusion.

## Current State (v2 Assessment)

`animation_player.h` contains explicit deprecation markers:

```c
/**
 * @deprecated Use play_scheduler_next/play_scheduler_prev instead
 */
void animation_player_cycle_animation(bool forward);

/**
 * @deprecated Use play_scheduler_play_named_channel instead
 */
esp_err_t animation_player_request_swap_current(void);
```

Additionally, there are likely other deprecated or unused APIs across the codebase.

## Inventory of Deprecated APIs

### Confirmed Deprecated (in animation_player.h)

| API | Replacement | Status |
|-----|-------------|--------|
| `animation_player_cycle_animation()` | `play_scheduler_next/prev()` | Marked deprecated |
| `animation_player_request_swap_current()` | `play_scheduler_play_named_channel()` | Marked deprecated |

### Likely Candidates (need verification)

| API | Location | Notes |
|-----|----------|-------|
| `app_state` functions | `app_state/` | Superseded by `p3a_state` |
| Old channel switching | `makapix.h` | May have redundant paths |
| Legacy playlist APIs | `channel_manager/` | Superseded by `play_scheduler` |

## Removal Process

### Step 1: Audit Callers

Use ripgrep to find all callers:

```bash
rg "animation_player_cycle_animation" --type c
rg "animation_player_request_swap_current" --type c
```

### Step 2: Migrate Callers

For each caller, replace with the modern API:

```c
// Before
animation_player_cycle_animation(true);  // forward

// After
play_scheduler_next(NULL);
```

```c
// Before
animation_player_request_swap_current();

// After
play_scheduler_play_named_channel("sdcard");  // or current channel
```

### Step 3: Add Compile-Time Warning

Before removal, add warnings to catch any remaining usage:

```c
__attribute__((deprecated("Use play_scheduler_next() instead")))
void animation_player_cycle_animation(bool forward);
```

### Step 4: Remove Implementation

After all callers migrated and tested:

1. Remove function body from `animation_player.c`
2. Remove declaration from `animation_player.h`
3. Update any documentation referencing the old API

### Step 5: Audit for Similar Patterns

Search for other deprecated patterns:

```bash
rg "@deprecated" --type c
rg "DEPRECATED" --type c
rg "TODO.*remove" --type c
```

## app_state Component Review

The `app_state` component (`components/app_state/`) may be entirely superseded by `p3a_state`:

### Current app_state API

```c
// app_state.h
typedef enum {
    APP_STATE_READY,
    APP_STATE_PROCESSING,
    APP_STATE_ERROR
} app_state_t;

app_state_t app_state_get(void);
void app_state_set(app_state_t state);
```

### Analysis

- `p3a_state.h` defines more comprehensive states
- `app_state` may be legacy from earlier development
- Check if any code depends on `app_state`

### Action

```bash
rg "app_state_" --type c --type h
```

If no meaningful usage, consider:
1. Mark component as deprecated
2. Remove from `main/CMakeLists.txt` REQUIRES
3. Delete component after verification

## Migration Checklist

### animation_player_cycle_animation

- [ ] Search all callers
- [ ] Replace each with `play_scheduler_next/prev`
- [ ] Add deprecation attribute
- [ ] Build and test
- [ ] Remove function
- [ ] Remove from header

### animation_player_request_swap_current

- [ ] Search all callers
- [ ] Replace each with appropriate play_scheduler call
- [ ] Add deprecation attribute
- [ ] Build and test
- [ ] Remove function
- [ ] Remove from header

### app_state component (if deprecated)

- [ ] Audit all usage
- [ ] Migrate to p3a_state
- [ ] Remove from REQUIRES
- [ ] Delete component files

## Success Criteria

- [ ] No deprecated functions in public headers
- [ ] No callers of removed functions
- [ ] Build succeeds without warnings
- [ ] Documentation updated
- [ ] Unused components removed

## Risks

| Risk | Mitigation |
|------|------------|
| Breaking functionality | Thorough caller audit |
| Hidden callers via macros | Search for function name substrings |
| External users of API | This is internal firmware; no external API |
