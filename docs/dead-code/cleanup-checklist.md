# Dead Code Cleanup Checklist

A prioritized list of cleanup tasks for dead code removal.

---

## Quick Wins (5-10 minutes each)

### 1. Remove Unused Image Assets

```bash
# Files to delete:
rm images/PICO-8_logo.png
rm images/PICO-8_logo_35p.gif
rm webui/static/pico8_logo.png
```

**Impact:** Reduces repository size  
**Risk:** None - files have no references

---

### 2. Fix Duplicate Declaration

**File:** `main/include/animation_player.h`

The function `animation_player_request_swap_current()` is declared at both line 93 and line 113.

Remove lines 92-93:
```c
// Remove this duplicate:
/**
 * @deprecated Use play_scheduler_play_named_channel instead
 */
esp_err_t animation_player_request_swap_current(void);
```

Keep lines 104-113 which have the correct documentation.

---

## Medium Effort (30-60 minutes each)

### 4. Remove Unused `content_source` Component

**Directory:** `components/content_source/`

1. First, verify no references exist:
   ```bash
   # Verify the component is truly unused
   grep -r "content_source" --include="*.c" --include="*.h" main/ components/ | grep -v "content_source/"
   ```

2. If no references found, delete the directory:
   ```bash
   # Preview what will be deleted
   ls -la components/content_source/
   
   # Delete after verification
   rm -rf components/content_source/
   ```

3. Check if any CMakeLists.txt references it (none found in analysis)

4. Update any documentation referencing this component (only found in `docs/first-principles/v2/high-level/03-content-pipeline-refactoring.md`)

---

### 5. Clean Up Unused Kconfig Option

**File:** `components/play_scheduler/Kconfig`

Either:
- **Remove** the `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW` option
- **Implement** its usage in `play_scheduler_pick.c` for RandomPick mode

---

### 6. Add Deprecation Attributes

**Files to update:**
- `components/channel_manager/sdcard_channel_impl.c`
- `components/channel_manager/makapix_channel_impl.c`

Add explicit deprecation warnings to navigation functions:

```c
// Before
static esp_err_t sdcard_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item);

// After
__attribute__((deprecated("Use play_scheduler_next() instead")))
static esp_err_t sdcard_impl_next_item(channel_handle_t channel, channel_item_ref_t *out_item);
```

---

## Low Priority (When Refactoring)

### 7. Archive Old Releases

**Directory:** `release/`

Consider moving old releases (v0.6.x, early v0.7.x) to a separate archive or removing them:

```bash
# First, list what will be moved
ls -d release/v0.6.* release/v0.7.0-dev release/v0.7.1-dev 2>/dev/null

# Create archive and move (after verifying the list above)
mkdir -p release/archive
mv release/v0.6.0-dev release/archive/
mv release/v0.6.1-dev release/archive/
mv release/v0.6.2-dev release/archive/
mv release/v0.6.3-dev release/archive/
mv release/v0.6.4-dev release/archive/
mv release/v0.6.5-dev release/archive/
mv release/v0.7.0-dev release/archive/
mv release/v0.7.1-dev release/archive/
```

**Note:** Verify the exact list of release directories before moving. The versions above are examples based on the analysis.

---

### 8. Remove Legacy Migration Code (After Transition Period)

**File:** `components/channel_manager/channel_cache.c`

Functions:
- `is_legacy_format()`
- `load_legacy_format()`

These can be removed once all users have migrated to the new format (consider adding telemetry or version check).

---

## Do NOT Remove

The following items look like dead code but should be kept:

| Item | Reason |
|------|--------|
| `__attribute__((unused))` functions | Intentional debug/development helpers |
| `#if 0` blocks in ugfx | Third-party upstream code |
| Commented code in gif.inl | Third-party upstream code |
| pico8_stream_stubs.c | Provides no-op implementations for disabled feature |
| debug_http_log component | Proper feature gating with CONFIG_P3A_PERF_DEBUG |

---

## Validation Checklist

After any cleanup, verify:

- [ ] `idf.py build` succeeds
- [ ] No new warnings introduced
- [ ] Basic functionality still works (animation playback, touch, WiFi)
- [ ] OTA update mechanism unaffected
- [ ] HTTP API still responds
- [ ] PICO-8 streaming still works (if enabled)

---

## Estimated Total Cleanup Time

| Category | Tasks | Time |
|----------|-------|------|
| Quick Wins | 3 | 15-30 min |
| Medium Effort | 3 | 1.5-3 hours |
| Low Priority | 3 | 1-2 hours |
| **Total** | **9** | **3-5 hours** |
