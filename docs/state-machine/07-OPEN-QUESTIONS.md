# Open Questions and Design Decisions

## Overview

This document captures unresolved design questions that need to be answered before implementation begins. Each question includes analysis of options with recommendations.

---

## Connectivity State

### Q1: Internet Connectivity Check Endpoint

**Question**: What endpoint should we use for internet connectivity checks?

**Context**: We need a reliable way to determine if the device has internet access beyond just Wi-Fi connectivity. This check runs periodically (every 60s) and on Wi-Fi connect.

**Options**:

#### Option A: Google Captive Portal Check
```
URL: http://clients3.google.com/generate_204
Method: HEAD
Expected: 204 No Content
```

**Pros**:
- Widely used by Android/ChromeOS
- Very reliable uptime (Google infrastructure)
- Returns 204 (no content body, minimal bandwidth)
- Specifically designed for this purpose

**Cons**:
- Dependency on Google infrastructure
- Privacy implications (Google sees all checks)
- May be blocked in some regions/networks

#### Option B: Mozilla Captive Portal Check
```
URL: http://detectportal.firefox.com/success.txt
Method: GET
Expected: 200 OK, body: "success\n"
```

**Pros**:
- Used by Firefox and many Linux distributions
- Non-profit infrastructure (privacy-friendly)
- Simple text response

**Cons**:
- Slightly more bandwidth (text response)
- Less ubiquitous than Google
- May have lower uptime guarantees

#### Option C: Makapix Backend Ping
```
URL: http://api.makapix.club/ping
Method: HEAD
Expected: 200 OK
```

**Pros**:
- Verifies connectivity to Makapix specifically
- No external dependencies
- Can return server status

**Cons**:
- Single point of failure (if Makapix is down, internet appears down)
- Doesn't distinguish between internet issues and server issues
- Additional load on Makapix infrastructure

#### Option D: Multiple Endpoints with Fallback
```
Primary: http://clients3.google.com/generate_204
Fallback: http://detectportal.firefox.com/success.txt
```

**Pros**:
- Most reliable (redundancy)
- Works even if one service is down

**Cons**:
- More complex logic
- Takes longer (sequential checks on failure)

**Recommendation**: **Option A (Google)** with config option to switch to Option B or disable.

**Rationale**: Google's endpoint is the most widely tested and reliable. Privacy concerns are minimal (HEAD request with no user data). Making it configurable allows users to opt out.

**Implementation**:
```c
// In sdkconfig
CONFIG_P3A_INTERNET_CHECK_URL="http://clients3.google.com/generate_204"
CONFIG_P3A_INTERNET_CHECK_ENABLED=y

bool check_internet_connectivity(void) {
    #ifndef CONFIG_P3A_INTERNET_CHECK_ENABLED
    return true;  // Assume internet is available
    #endif
    
    esp_http_client_config_t config = {
        .url = CONFIG_P3A_INTERNET_CHECK_URL,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 5000,
    };
    // ... perform check ...
}
```

---

### Q2: MQTT Retry Backoff Strategy

**Question**: How aggressively should we retry MQTT connections after disconnect?

**Context**: MQTT can disconnect due to network issues, server maintenance, or authentication problems. We need a balance between quick reconnection and not hammering the server.

**Options**:

#### Option A: Exponential Backoff (Recommended)
```
Retry delays: 1s, 2s, 4s, 8s, 16s, 32s, max 60s
Reset on successful connection
```

**Implementation**:
```c
static uint32_t s_mqtt_retry_delay_ms = 1000;
static const uint32_t MAX_RETRY_DELAY_MS = 60000;

void on_mqtt_disconnected(void) {
    xTimerStart(s_retry_timer, pdMS_TO_TICKS(s_mqtt_retry_delay_ms));
    
    // Exponential backoff
    s_mqtt_retry_delay_ms *= 2;
    if (s_mqtt_retry_delay_ms > MAX_RETRY_DELAY_MS) {
        s_mqtt_retry_delay_ms = MAX_RETRY_DELAY_MS;
    }
}

void on_mqtt_connected(void) {
    s_mqtt_retry_delay_ms = 1000;  // Reset
}
```

**Pros**:
- Quick reconnection on transient issues (1s first retry)
- Backs off on persistent problems (avoids server load)
- Standard pattern (used by many clients)

**Cons**:
- Complex logic
- May wait too long if issue resolves quickly

#### Option B: Fixed Interval
```
Retry every 10 seconds indefinitely
```

**Pros**:
- Simple implementation
- Predictable behavior

**Cons**:
- May be too aggressive if server is having issues
- May be too slow if issue is transient

#### Option C: Progressive with Caps
```
Retry delays: 5s, 10s, 30s, 60s, then every 60s
```

**Pros**:
- Balance between quick and cautious
- Simpler than exponential

**Cons**:
- Arbitrary values
- Less standard than exponential

**Recommendation**: **Option A (Exponential Backoff)**

**Rationale**: Industry standard, responsive for transient issues, polite for persistent issues.

---

### Q3: Transient State Display

**Question**: Should we show transient connecting states or only problems?

**Context**: When MQTT is connecting, should we show "Connecting to Makapix..." or stay silent until there's a problem?

**Options**:

#### Option A: Show Transient States (Verbose)
```
States shown:
- "Connecting to Makapix..."
- "Reconnecting to Makapix..."
- "Lost connection, retrying..."
```

**Pros**:
- User always knows what's happening
- Helpful for debugging
- Reduces support questions

**Cons**:
- Can be distracting
- Clutters screen during normal operation

#### Option B: Show Only Problems (Clean)
```
States shown:
- (nothing while connecting)
- "No Internet" (after timeout)
- "Connection failed" (after multiple retries)
```

**Pros**:
- Cleaner UX
- Less notification fatigue
- Only interrupts for real problems

**Cons**:
- User may wonder why nothing is happening
- Harder to diagnose issues

#### Option C: Hybrid (Configurable)
```
Default: Show transient states
Config option: Hide transient states
```

**Implementation**:
```c
// In sdkconfig
CONFIG_P3A_SHOW_TRANSIENT_MESSAGES=y

const char* get_user_message(void) {
    if (!mqtt_connected) {
        #ifdef CONFIG_P3A_SHOW_TRANSIENT_MESSAGES
        return "Connecting to Makapix...";
        #else
        return NULL;  // Silent until timeout
        #endif
    }
    // ...
}
```

**Recommendation**: **Option A initially, Option C for production**

**Rationale**: Show all states during beta testing to catch issues. Add config option to hide transient states for users who find them annoying.

---

## Channel Index (LAi)

### Q4: LAi Sorting Strategy

**Question**: Should LAi be sorted by Ci index, or left in insertion order?

**Context**: LAi contains indices into Ci. Sorting enables binary search for `lai_contains()`, but adds complexity.

**Options**:

#### Option A: Sorted by Ci Index
```c
// LAi is always sorted ascending by Ci index
// [0, 5, 12, 23, 45, ...]

bool lai_contains(ps_channel_state_t *ch, uint32_t ci_idx) {
    // Binary search - O(log n)
    return binary_search(ch->lai_indices, ch->lai_count, ci_idx);
}

void lai_add(ps_channel_state_t *ch, uint32_t ci_idx) {
    // Insert in sorted position - O(n)
    uint32_t pos = find_insert_pos(ch->lai_indices, ch->lai_count, ci_idx);
    memmove(&ch->lai_indices[pos + 1], &ch->lai_indices[pos], ...);
    ch->lai_indices[pos] = ci_idx;
}
```

**Pros**:
- Fast lookup: O(log n) for `lai_contains()`
- Predictable order (matches Ci order)
- Good for debugging (easy to spot missing entries)

**Cons**:
- Slower insertion: O(n) due to memmove
- More complex implementation

#### Option B: Insertion Order (Unsorted)
```c
// LAi in insertion order
// [23, 0, 45, 5, 12, ...]

bool lai_contains(ps_channel_state_t *ch, uint32_t ci_idx) {
    // Linear search - O(n)
    for (uint32_t i = 0; i < ch->lai_count; i++) {
        if (ch->lai_indices[i] == ci_idx) return true;
    }
    return false;
}

void lai_add(ps_channel_state_t *ch, uint32_t ci_idx) {
    // Append - O(1)
    ch->lai_indices[ch->lai_count++] = ci_idx;
}
```

**Pros**:
- Fast insertion: O(1) append
- Simple implementation
- Preserves download order (may be useful for metrics)

**Cons**:
- Slower lookup: O(n) linear search
- Unpredictable order (harder to debug)

**Performance Analysis**:

For typical LAi of 100 entries:
- **Binary search**: ~7 comparisons
- **Linear search**: ~50 comparisons (average)

For large LAi of 1000 entries:
- **Binary search**: ~10 comparisons
- **Linear search**: ~500 comparisons (average)

**Recommendation**: **Option A (Sorted)**

**Rationale**: `lai_contains()` is called during download scanning (checking if entry is already available). With thousands of entries, O(log n) vs O(n) makes a significant difference. Insertion is less frequent and can tolerate O(n) cost.

---

### Q5: LAi Rebuild Frequency

**Question**: How often should we rebuild LAi from scratch (full filesystem scan)?

**Context**: LAi can become stale if files are added/removed externally (USB mass storage). Rebuilding is expensive (stat() every entry).

**Options**:

#### Option A: On Boot + SD Card Operations Only
```
Rebuild triggers:
- System boot
- SD card mount/unmount
- User-initiated "refresh cache" button
```

**Pros**:
- Minimal performance impact
- Predictable (only on major events)

**Cons**:
- Stale LAi if files added via USB
- Playback errors reveal stale entries

#### Option B: Periodic Rebuild
```
Rebuild triggers:
- Option A triggers
- Every 24 hours (background task)
```

**Pros**:
- Catches external changes eventually
- Self-healing

**Cons**:
- Periodic performance impact
- Arbitrary 24h interval

#### Option C: Never Rebuild (Incremental Only)
```
Rebuild triggers:
- Never (rely on incremental updates)
- Playback errors trigger removal from LAi
```

**Pros**:
- Best performance
- Self-healing on playback errors

**Cons**:
- User may see errors before self-healing
- Files added externally never detected

#### Option D: Hybrid (On-Demand + Self-Healing)
```
Rebuild triggers:
- Option A triggers
- On playback error (remove stale entry)
- On download (check if file already exists)
```

**Pros**:
- Good performance
- Self-healing
- Discovers shared files

**Cons**:
- Most complex

**Recommendation**: **Option D (Hybrid)**

**Rationale**: Combines best of all approaches. Self-healing handles stale entries, download checks discover shared files, manual triggers cover major events.

**Implementation**:
```c
// On playback error
if (!file_exists(filepath)) {
    ESP_LOGW(TAG, "File missing (stale LAi) - removing entry");
    ps_lai_remove(ch, ci_idx);
}

// On download (before HTTP request)
if (file_exists(filepath)) {
    ESP_LOGI(TAG, "File already exists (shared) - skipping download");
    ps_lai_add(ch, ci_idx);
    return ESP_OK;
}
```

---

## Download Coordination

### Q6: Download Bandwidth Limiting

**Question**: Should we limit download bandwidth to avoid saturating Wi-Fi?

**Context**: Aggressive downloading may impact other devices on the network or cause MQTT latency.

**Options**:

#### Option A: No Limit (Greedy)
```c
// Download at full Wi-Fi speed
while (find_next_download(&req)) {
    perform_download(&req);
}
```

**Pros**:
- Fastest channel filling
- Simple implementation
- Makes use of available bandwidth

**Cons**:
- May saturate Wi-Fi
- May impact MQTT responsiveness
- May annoy other network users

#### Option B: Hard Limit
```c
// Limit to 1 MB/s (configurable)
CONFIG_P3A_DOWNLOAD_RATE_LIMIT_KBPS=1024

while (find_next_download(&req)) {
    uint32_t start = get_time_ms();
    perform_download(&req);
    uint32_t duration = get_time_ms() - start;
    
    uint32_t bytes = get_file_size(req.filepath);
    uint32_t rate_kbps = (bytes * 8) / duration;
    
    if (rate_kbps > CONFIG_P3A_DOWNLOAD_RATE_LIMIT_KBPS) {
        uint32_t delay = calculate_delay(bytes, rate_kbps, CONFIG_P3A_DOWNLOAD_RATE_LIMIT_KBPS);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}
```

**Pros**:
- Predictable bandwidth usage
- Configurable per-device

**Cons**:
- Complex implementation
- May under-utilize bandwidth
- Arbitrary limit value

#### Option C: Adaptive (Monitor MQTT Latency)
```c
// Slow down downloads if MQTT latency increases
while (find_next_download(&req)) {
    perform_download(&req);
    
    uint32_t mqtt_latency = get_mqtt_latency_ms();
    if (mqtt_latency > 500) {
        // MQTT is getting slow - pause downloads
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        // Small delay between downloads
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

**Pros**:
- Self-adjusting
- Prioritizes MQTT

**Cons**:
- Complex to implement
- May over-react to transient spikes

**Recommendation**: **Option A with config option for Option B**

**Rationale**: Start with no limit (most users have adequate Wi-Fi). Add config option for users who experience issues. Monitor for complaints before adding adaptive logic.

**Implementation**:
```c
// In sdkconfig
CONFIG_P3A_DOWNLOAD_RATE_LIMIT_ENABLED=n
CONFIG_P3A_DOWNLOAD_RATE_LIMIT_KBPS=1024
```

---

### Q7: Download Priority Strategy

**Question**: Should download manager prioritize channels with fewer available artworks?

**Context**: Simple round-robin treats all channels equally. Prioritizing channels with smaller LAi ensures variety faster.

**Options**:

#### Option A: Simple Round-Robin (Current)
```c
// Rotate through channels in order
size_t ch_idx = (s_round_robin_idx++) % s_channel_count;
```

**Pros**:
- Simple implementation
- Fair distribution
- Predictable

**Cons**:
- Channels with 0 artworks wait as long as channels with many

#### Option B: Priority to Small LAi
```c
// Sort channels by lai_count ascending
// Download from channels with fewer entries first
size_t find_channel_to_download(void) {
    size_t best_idx = 0;
    uint32_t min_lai = UINT32_MAX;
    
    for (size_t i = 0; i < s_channel_count; i++) {
        uint32_t lai_count = get_lai_count(s_channels[i].channel_id);
        if (lai_count < min_lai && has_missing_files(i)) {
            min_lai = lai_count;
            best_idx = i;
        }
    }
    
    return best_idx;
}
```

**Pros**:
- Fills empty channels first
- Ensures variety quickly
- Better user experience (less "no artworks" time)

**Cons**:
- More complex
- May starve channels with many artworks

#### Option C: Weighted Round-Robin
```c
// Give more slots to channels with smaller LAi
uint32_t weight = MAX_ENTRIES - ch->lai_count;
```

**Pros**:
- Balance between priority and fairness
- Smoother than strict priority

**Cons**:
- Most complex
- May be overkill

**Recommendation**: **Option B (Priority to Small LAi)**

**Rationale**: User experience is better when variety appears quickly. Channels with 0 artworks should be filled before adding to channels with 50 artworks.

**Refinement**: After all channels have at least 5 artworks, switch to round-robin.

```c
size_t find_channel_to_download(void) {
    // Phase 1: Fill until all channels have min_threshold artworks
    const uint32_t MIN_THRESHOLD = 5;
    
    for (size_t i = 0; i < s_channel_count; i++) {
        if (get_lai_count(i) < MIN_THRESHOLD && has_missing_files(i)) {
            return i;
        }
    }
    
    // Phase 2: Round-robin after threshold met
    return (s_round_robin_idx++) % s_channel_count;
}
```

---

## User Feedback

### Q8: Message Auto-Dismiss Behavior

**Question**: Should informative messages auto-dismiss after a timeout?

**Context**: Messages like "Downloading artwork..." may persist for minutes. Should they auto-dismiss or stay until state changes?

**Options**:

#### Option A: No Auto-Dismiss
```
Message stays until state changes
```

**Pros**:
- Truthful (message reflects current state)
- No surprise transitions

**Cons**:
- May feel stuck if download is slow

#### Option B: Auto-Dismiss with Resume
```
Message stays for 10 seconds, then fades
If state hasn't changed, resume artwork playback
Show subtle indicator (corner icon) that operation continues
```

**Pros**:
- Less intrusive
- Screen doesn't feel "stuck"

**Cons**:
- User may think operation stopped
- Requires resumable playback

#### Option C: Tappable Dismiss
```
Message stays until:
- State changes, OR
- User taps screen to dismiss
```

**Pros**:
- User control
- Still accurate

**Cons**:
- User may not know they can dismiss
- Requires touch handling

**Recommendation**: **Option A (No Auto-Dismiss)**

**Rationale**: Messages represent current state. Auto-dismissing would be misleading. If downloads are taking too long, the real fix is faster downloads, not hiding the message.

**Future Enhancement**: Add "estimated time remaining" to download messages.

---

### Q9: Progress Bar vs Spinner

**Question**: For loading/downloading states, show progress bar or spinner?

**Context**: Progress bars require knowing total work. Spinners are simpler but less informative.

**Options**:

#### Option A: Progress Bar (When Available)
```
Downloading artwork
████████░░ 78%
```

**Pros**:
- Most informative
- Shows estimated completion
- Reduces perceived wait time

**Cons**:
- Not always possible (unknown total)
- More complex to implement

#### Option B: Spinner Always
```
Downloading artwork
   ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
```

**Pros**:
- Simple implementation
- Works for any duration

**Cons**:
- Less informative
- Can't estimate completion

#### Option C: Hybrid
```
Progress bar for downloads (known size)
Spinner for channel refresh (unknown duration)
```

**Recommendation**: **Option C (Hybrid)**

**Rationale**: Use the best visualization for each situation. Downloads have progress, refreshes don't.

**Implementation**:
```c
// For downloads
display_progress_bar("Downloading artwork", percent);

// For refresh
display_spinner("Loading channel");
```

---

## Summary of Recommendations

| Question | Recommendation | Rationale |
|----------|---------------|-----------|
| Internet Check Endpoint | Google Captive Portal | Most reliable, widely tested |
| MQTT Retry Backoff | Exponential (1s to 60s) | Industry standard, balanced |
| Transient State Display | Show initially, config later | Helpful for debugging |
| LAi Sorting | Sorted by Ci index | O(log n) lookup is worth it |
| LAi Rebuild Frequency | Hybrid (on-demand + self-healing) | Best performance + accuracy |
| Download Bandwidth | No limit + config option | Start simple, add if needed |
| Download Priority | Priority to small LAi | Better UX (variety faster) |
| Message Auto-Dismiss | No auto-dismiss | Truthful, accurate |
| Progress Visualization | Hybrid (bar for downloads, spinner for refresh) | Best of both |

## Implementation Order

Implement in this order to minimize risk:

1. **LAi Support** (foundational, affects everything)
2. **Connectivity State** (enables online/offline mode)
3. **Play Scheduler Integration** (uses LAi)
4. **Download Manager** (fills LAi)
5. **User Messaging** (polish, can iterate)

## Deferred Questions

These questions can be answered during or after implementation:

- Haptic feedback (requires hardware capability check)
- Multi-language support (future i18n work)
- Advanced download scheduling (optimize for Wi-Fi off-peak)
- LRU eviction strategy refinement (needs real-world usage data)

## Decision Log

When decisions are made, update this table:

| Date | Question | Decision | Decided By | Notes |
|------|----------|----------|------------|-------|
| TBD | Internet Check | Option A (Google) | TBD | Pending approval |
| TBD | MQTT Retry | Option A (Exponential) | TBD | - |
| ... | ... | ... | ... | ... |

---

**Next Steps:**
1. Review recommendations with team
2. Make final decisions on each question
3. Update implementation plan with decisions
4. Begin Phase 1 implementation
