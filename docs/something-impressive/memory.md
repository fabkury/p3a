# Memory & Resource Audit

Memory leaks, resource leaks, buffer issues, and stack risks.

---

## Critical — Verified

### 1. HTTP Upload Buffer Leak on Error Paths — FALSE POSITIVE

**File:** `components/http_api/http_api_upload.c:146-154, 358-361`

**Reported:** `recv_buf` leaked in multiple error paths.

**Verification:** All error paths inside the receive loop use `break`, which falls
through to cleanup at lines 358-361 where `recv_buf` is always freed. No leak
exists.

---

### 2. PNG Decoder `rgb_buffer` Leak — REAL BUG (FIXED)

**File:** `components/animation_decoder/png_animation_decoder.c:227-232`

**Original report** incorrectly described this as a missing `free(dec)` at lines
191-199. In reality, `dec` is not allocated until line 227 — well after the
row_pointers check.

**Actual bug:** When `calloc` for `dec` fails at line 227, the error path freed
`rgba_buffer` and `png_data` but **not** `rgb_buffer`. Since `has_transparency`
is mutually exclusive, exactly one of rgba/rgb is non-NULL, so the other
`free()` is a no-op. The fix adds `free(png_data->rgb_buffer)` to match the
cleanup pattern already used at lines 194-195.

**Status:** Fixed.

---

## High — Verified

### 3. Download Chunk Buffer Leak on HTTP Init Failure — FALSE POSITIVE

**Files:** `components/giphy/giphy_download.c:208-236`,
`components/channel_manager/makapix_artwork.c:227-252`

**Reported:** `chunk_buffer` not freed when `esp_http_client_init()` fails.

**Verification:** `giphy_download.c:227` and `makapix_artwork.c:250` both free
`chunk_buffer` in the HTTP init failure path. No leak exists.

---

### 4. Filepath Leak on Decoder Init Failure — FALSE POSITIVE

**File:** `main/animation_player_loader.c:228-234`

**Reported:** `buf->filepath` not freed on decoder init failure.

**Verification:** `animation_player_loader.c:830` frees `filepath` in the
cleanup path. No leak exists.

---

### 5. Playset Channel Details Leak — FALSE POSITIVE

**File:** `components/http_api/http_api_rest_playsets.c:226-229`

**Reported:** `ch_details` not freed on partial failure.

**Verification:** `http_api_rest_playsets.c:245` always frees `ch_details`. No
leak exists.

---

## Medium

### 6. Unchecked `snprintf` Return Values

**File:** `main/animation_player_loader.c:116, 393-397, 452`

`snprintf()` return values are not checked for truncation:

```c
snprintf(temp_path, sizeof(temp_path), "%s/upload_%llu.tmp", DOWNLOADS_DIR,
         (unsigned long long)(esp_timer_get_time() / 1000));
```

If `DOWNLOADS_DIR` path is long, the timestamp is silently truncated. Same issue
at lines 393 (`full_path[512]`) and 452 (`subdir_path[512]`) with recursive
subdirectory paths.

**Impact:** Silent path truncation leading to file operations on wrong paths —
potential corruption or security issues.

**Fix:** Check `ret >= sizeof(buf)` and return an error on truncation.

---

### 7. Stack Overflow Risk in Directory Traversal

**File:** `main/animation_player_loader.c:393, 452`

`find_animations_directory()` uses recursive calls with 512-byte local arrays on
each frame:

```c
char full_path[512];
char subdir_path[512];
```

Combined with other local variables and nested function calls, deep directory
trees can overflow the default 8 KB task stack.

**Impact:** Stack corruption leading to hard crashes. Rare but catastrophic.

**Fix:** Move large arrays to heap allocation, or iterate instead of recursing.

---

### 8. Integer Overflow in Upscale Lookup Allocation

**File:** `main/animation_player_loader.c:621, 628`

Upscale lookup table allocation multiplies dimensions without overflow check:

```c
buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc(
    (size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
```

A malicious image with extreme dimensions could cause an integer overflow in the
size calculation.

**Impact:** Undersized allocation leading to heap buffer overflow during
rendering.

**Fix:** Add explicit overflow check:
```c
if (max_len > SIZE_MAX / sizeof(uint16_t)) return ESP_ERR_NO_MEM;
```
