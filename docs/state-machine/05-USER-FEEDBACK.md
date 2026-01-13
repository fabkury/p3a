# User Feedback Specification

## Overview

This document specifies how p3a keeps users informed about what's happening at all times, particularly during states where no artwork is being displayed.

## Core Principle

> **Users should never be confused about what p3a is doing.** Every state should have clear, concise messaging that explains the current situation and (when appropriate) what action the user can take.

## Message Hierarchy

Messages are displayed in priority order. Higher priority messages override lower priority ones.

### Priority Levels

```
Priority 1: Critical Connectivity Issues
  ├─ No Wi-Fi
  ├─ No Internet  
  └─ Not Registered

Priority 2: Transient Operational States
  ├─ Connecting to Makapix
  ├─ Loading channel
  └─ Downloading artwork

Priority 3: Channel State Issues
  ├─ Channel empty
  ├─ No artworks available
  └─ Download failed

Priority 4: Normal Operation
  └─ (No message, artwork playing)
```

## Message Templates

### Connectivity Messages (Priority 1)

#### No Wi-Fi
```
╔══════════════════════════╗
║     No Wi-Fi             ║
║                          ║
║  Long-press to configure ║
╚══════════════════════════╝
```

**When**: `!p3a_connectivity_has_wifi()`

**User Action**: Long-press activates captive portal for Wi-Fi setup.

#### No Internet
```
╔══════════════════════════╗
║   No Internet Access     ║
║                          ║
║ Check your Wi-Fi router  ║
╚══════════════════════════╝
```

**When**: `p3a_connectivity_has_wifi() && !p3a_connectivity_has_internet()`

**User Action**: Check router, wait for internet to restore.

#### Not Registered
```
╔══════════════════════════╗
║    Not Registered        ║
║                          ║
║ Long-press to register   ║
║   with Makapix Club      ║
╚══════════════════════════╝
```

**When**: `p3a_connectivity_has_internet() && !p3a_connectivity_is_registered()`

**User Action**: Long-press starts provisioning flow.

### Operational Messages (Priority 2)

#### Connecting to Makapix
```
╔══════════════════════════╗
║  Connecting to Makapix   ║
║                          ║
║         [⋯⋯⋯]           ║
╚══════════════════════════╝
```

**When**: `p3a_connectivity_is_registered() && !p3a_connectivity_has_mqtt()`

**Duration**: Typically 1-5 seconds.

**User Action**: Wait.

#### Loading Channel
```
╔══════════════════════════╗
║   Loading channel...     ║
║                          ║
║     [promoted]           ║
║                          ║
║  123 artworks received   ║
╚══════════════════════════╝
```

**When**: Channel refresh in progress, LAi empty.

**Details**:
- Channel name
- Number of entries received so far (live updates)

**User Action**: Wait for refresh to complete.

#### Downloading Artwork
```
╔══════════════════════════╗
║  Downloading artwork     ║
║                          ║
║       ████████░░ 78%     ║
╚══════════════════════════╝
```

**When**: Download in progress for first artwork after empty LAi.

**Details**:
- Progress bar (if download progress available)
- Percentage

**User Action**: Wait for download to complete.

### Channel State Messages (Priority 3)

#### Channel Empty
```
╔══════════════════════════╗
║    Channel Empty         ║
║                          ║
║ This channel has no      ║
║  artworks yet            ║
╚══════════════════════════╝
```

**When**: Refresh completed but `entry_count == 0`.

**User Action**: Try a different channel (tap to navigate).

#### No Artworks Available
```
╔══════════════════════════╗
║  No Artworks Available   ║
║                          ║
║  Long-press to register  ║
║    or add files via USB  ║
╚══════════════════════════╝
```

**When**: All LAi arrays empty, no refresh in progress, no download active.

**User Action**: Register with Makapix or add files to SD card.

#### Download Failed
```
╔══════════════════════════╗
║   Download Failed        ║
║                          ║
║  Tap to retry            ║
╚══════════════════════════╝
```

**When**: Download failed with transient error (503, 429, network timeout).

**User Action**: Tap to trigger retry.

**Note**: Permanent failures (404) don't show this message.

## Message Logic Flow

```c
const char* get_user_message(char *detail_buf, size_t detail_len) {
    // Priority 1: Connectivity
    if (!p3a_connectivity_has_wifi()) {
        return "No Wi-Fi\n\nLong-press to configure";
    }
    
    if (!p3a_connectivity_has_internet()) {
        return "No Internet Access\n\nCheck your Wi-Fi router";
    }
    
    if (!p3a_connectivity_is_registered()) {
        return "Not Registered\n\nLong-press to register\nwith Makapix Club";
    }
    
    // Priority 2: Operational
    if (p3a_connectivity_is_registered() && !p3a_connectivity_has_mqtt()) {
        return "Connecting to Makapix...";
    }
    
    // Check if any LAi has entries
    bool any_lai_available = false;
    bool any_refresh_active = false;
    for (size_t i = 0; i < channel_count; i++) {
        if (channels[i].lai_count > 0) {
            any_lai_available = true;
        }
        if (channels[i].refresh_in_progress) {
            any_refresh_active = true;
            // Build detail message
            snprintf(detail_buf, detail_len, 
                     "Loading %s\n%lu artworks received",
                     channels[i].channel_id,
                     (unsigned long)channels[i].received_count);
        }
    }
    
    if (any_lai_available) {
        return NULL;  // Normal playback, no message
    }
    
    if (any_refresh_active) {
        return detail_buf;  // "Loading channel..." with details
    }
    
    // Check download status
    if (download_manager_is_busy()) {
        int progress = download_manager_get_progress();
        if (progress >= 0) {
            snprintf(detail_buf, detail_len,
                     "Downloading artwork\n\n%d%%", progress);
        } else {
            snprintf(detail_buf, detail_len,
                     "Downloading artwork...");
        }
        return detail_buf;
    }
    
    // Priority 3: Channel issues
    bool any_channel_empty = false;
    for (size_t i = 0; i < channel_count; i++) {
        if (channels[i].entry_count == 0 && !channels[i].refresh_in_progress) {
            any_channel_empty = true;
            break;
        }
    }
    
    if (any_channel_empty) {
        return "Channel Empty\n\nThis channel has no\nartworks yet";
    }
    
    // Truly no artworks
    return "No Artworks Available\n\nLong-press to register\nor add files via USB";
}
```

## Display Integration

### Animation Player Message Mode

```c
// In display_renderer.c
void render_message_screen(const char *title, const char *body) {
    // Clear screen to background color
    fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, BG_COLOR);
    
    // Draw title (large font)
    draw_text_centered(title, SCREEN_WIDTH/2, 200, FONT_LARGE, FG_COLOR);
    
    // Draw body (medium font)
    draw_text_centered_multiline(body, SCREEN_WIDTH/2, 300, FONT_MEDIUM, FG_COLOR);
    
    // Optional: Draw icon based on message type
    if (strstr(title, "Wi-Fi")) {
        draw_icon(ICON_WIFI_OFF, SCREEN_WIDTH/2, 100);
    } else if (strstr(title, "Downloading")) {
        draw_icon(ICON_DOWNLOAD, SCREEN_WIDTH/2, 100);
    }
    
    swap_buffers();
}
```

### Animated Transitions

```c
// Fade out old message, fade in new message
void transition_message(const char *old_msg, const char *new_msg) {
    for (int alpha = 255; alpha >= 0; alpha -= 15) {
        render_message_with_alpha(old_msg, alpha);
        vTaskDelay(pdMS_TO_TICKS(16));  // ~60 FPS
    }
    
    for (int alpha = 0; alpha <= 255; alpha += 15) {
        render_message_with_alpha(new_msg, alpha);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
```

**User Experience**: Smooth transitions between states, not jarring flashes.

## Web UI Status Integration

### Status Endpoint

```json
// GET /api/status
{
    "connectivity": {
        "wifi": true,
        "internet": true,
        "registered": true,
        "mqtt": true,
        "online_mode": true
    },
    "playback": {
        "state": "playing",  // or "loading", "downloading", "empty"
        "current_artwork": {
            "post_id": 12345,
            "storage_key": "abc-123",
            "channel": "promoted"
        },
        "message": null  // or message text if not playing
    },
    "channels": [
        {
            "channel_id": "promoted",
            "entry_count": 1024,
            "lai_count": 156,
            "refreshing": false,
            "download_progress": {
                "downloading": false,
                "current_file": null,
                "files_remaining": 868
            }
        }
    ],
    "download": {
        "active": true,
        "channel_id": "promoted",
        "storage_key": "xyz-789",
        "progress_percent": 45,
        "queue_size": 127
    }
}
```

**No Heavy Operations**: All data from in-memory state, no file scanning.

### Web UI Display

```html
<div class="status-panel">
    <h3>Connection Status</h3>
    <div class="status-item">
        <span class="status-icon">📶</span>
        <span>Wi-Fi: Connected</span>
    </div>
    <div class="status-item">
        <span class="status-icon">🌐</span>
        <span>Internet: Available</span>
    </div>
    <div class="status-item">
        <span class="status-icon">☁️</span>
        <span>Makapix: Connected</span>
    </div>
</div>

<div class="playback-panel">
    <h3>Playback</h3>
    <div class="playback-state">
        <span>▶️ Playing</span>
        <span>Post #12345</span>
    </div>
</div>

<div class="channels-panel">
    <h3>Channels</h3>
    <div class="channel-item">
        <span>promoted</span>
        <span>156 / 1024 available</span>
        <div class="progress-bar" style="width: 15%"></div>
    </div>
</div>
```

## Log Messages

### Log Levels

```c
// ESP_LOGE: Critical errors that require user attention
ESP_LOGE(TAG, "Failed to load channel cache: %s", esp_err_to_name(err));

// ESP_LOGW: Warnings that don't break functionality
ESP_LOGW(TAG, "Download failed (will retry): %s", storage_key);

// ESP_LOGI: Important state transitions
ESP_LOGI(TAG, "Channel refresh complete: %zu entries", entry_count);
ESP_LOGI(TAG, "First entry downloaded, starting playback");

// ESP_LOGD: Debug info for development
ESP_LOGD(TAG, "LAi cursor: %lu/%lu", cursor, lai_count);
```

### Structured Logs

For easier parsing and debugging:

```c
ESP_LOGI(TAG, "[PLAYBACK] state=LOADING channel=promoted entries_received=123");
ESP_LOGI(TAG, "[DOWNLOAD] action=START channel=promoted file=abc-123.webp");
ESP_LOGI(TAG, "[DOWNLOAD] action=COMPLETE duration_ms=2345 bytes=456789");
ESP_LOGI(TAG, "[CONNECTIVITY] wifi=UP internet=UP mqtt=DOWN");
```

## Debug Overlays (Development Only)

### On-Screen Debug Info

```c
#ifdef CONFIG_P3A_DEBUG_OVERLAY
void render_debug_overlay(void) {
    char buf[256];
    
    // Connectivity status
    snprintf(buf, sizeof(buf), "W:%d I:%d M:%d",
             p3a_connectivity_has_wifi(),
             p3a_connectivity_has_internet(),
             p3a_connectivity_has_mqtt());
    draw_text(buf, 10, 10, FONT_SMALL, DEBUG_COLOR);
    
    // Channel status
    snprintf(buf, sizeof(buf), "CH: %s LAi:%zu/%zu",
             current_channel_id, lai_count, entry_count);
    draw_text(buf, 10, 30, FONT_SMALL, DEBUG_COLOR);
    
    // Download status
    if (download_manager_is_busy()) {
        char channel[64];
        download_manager_get_active_channel(channel, sizeof(channel));
        snprintf(buf, sizeof(buf), "DL: %s (%d%%)", channel, dl_progress);
        draw_text(buf, 10, 50, FONT_SMALL, DEBUG_COLOR);
    }
    
    // Frame rate
    snprintf(buf, sizeof(buf), "FPS: %.1f", get_current_fps());
    draw_text(buf, 10, 70, FONT_SMALL, DEBUG_COLOR);
}
#endif
```

**Enable via**: `idf.py menuconfig` → p3a Configuration → Debug Overlay

## Touch Feedback

### Visual Feedback for Gestures

```c
void on_touch_detected(int x, int y) {
    // Show touch ripple effect
    for (int r = 10; r < 50; r += 5) {
        draw_circle(x, y, r, TOUCH_COLOR, false);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void on_long_press_detected(void) {
    // Show long-press indicator
    draw_text_centered("Long Press Detected", 
                       SCREEN_WIDTH/2, SCREEN_HEIGHT/2,
                       FONT_LARGE, HIGHLIGHT_COLOR);
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

### Haptic Feedback (Future)

If hardware supports haptic feedback:

```c
void haptic_feedback_light(void) {
    // Brief vibration for touch
    vibrate(50);  // 50ms
}

void haptic_feedback_heavy(void) {
    // Stronger vibration for long-press
    vibrate(200);  // 200ms
}
```

## Accessibility Considerations

### High Contrast Mode

```c
// For users with visual impairments
#ifdef CONFIG_P3A_HIGH_CONTRAST
#define BG_COLOR      0x000000  // Pure black
#define FG_COLOR      0xFFFFFF  // Pure white
#define HIGHLIGHT     0xFF0000  // Bright red
#else
#define BG_COLOR      0x1A1A1A  // Dark gray
#define FG_COLOR      0xE0E0E0  // Light gray
#define HIGHLIGHT     0x4A90E2  // Blue
#endif
```

### Large Text Mode

```c
#ifdef CONFIG_P3A_LARGE_TEXT
#define FONT_SMALL    16
#define FONT_MEDIUM   24
#define FONT_LARGE    36
#else
#define FONT_SMALL    12
#define FONT_MEDIUM   18
#define FONT_LARGE    28
#endif
```

## Message Persistence

### Minimum Display Time

```c
#define MIN_MESSAGE_DISPLAY_MS  1000  // Don't flash messages too quickly

static uint32_t s_message_shown_at = 0;

void show_message(const char *msg) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (s_current_message && s_message_shown_at > 0) {
        uint32_t elapsed = now - s_message_shown_at;
        if (elapsed < MIN_MESSAGE_DISPLAY_MS) {
            vTaskDelay(pdMS_TO_TICKS(MIN_MESSAGE_DISPLAY_MS - elapsed));
        }
    }
    
    s_current_message = msg;
    s_message_shown_at = xTaskGetTickCount() * portTICK_PERIOD_MS;
    render_message_screen(msg);
}
```

**Rationale**: Prevents flickering messages that are hard to read.

## Open Questions

### Q1: Message Auto-Dismiss
**Question**: Should messages auto-dismiss after a timeout, or stay until state changes?
**Options**:
- A) Auto-dismiss after 5 seconds
- B) Stay until state changes
- C) Hybrid: dismissable with tap, but no auto-dismiss

**Recommendation**: Option B. Messages represent current state, not transient notifications.

### Q2: Progress Animations
**Question**: Should loading/downloading messages have animated indicators?
**Options**:
- A) Static text only
- B) Spinner animation
- C) Progress bar (when progress available)

**Recommendation**: Option C with fallback to B. Progress bars are most informative.

### Q3: Multi-Channel Status
**Question**: When playing multiple channels, show per-channel status?
**Options**:
- A) Show only overall status
- B) Rotate through channel statuses
- C) Compact list of all channels

**Recommendation**: Option A on device, Option C in web UI. Device screen is too small for detailed multi-channel info.

## Implementation Checklist

- [ ] Add `p3a_connectivity_get_status_message()` to p3a_state.c
- [ ] Integrate message display into display_renderer.c
- [ ] Add progress tracking to download_manager.c
- [ ] Implement message transitions (fade in/out)
- [ ] Add debug overlay (ifdef CONFIG_P3A_DEBUG_OVERLAY)
- [ ] Update web UI /api/status endpoint
- [ ] Add touch feedback animations
- [ ] Test all message states manually
- [ ] Add accessibility config options
- [ ] Document message strings for i18n (future)

## Testing Strategy

### Manual Testing
- Disconnect Wi-Fi → verify "No Wi-Fi" message
- Reconnect Wi-Fi → verify "Connecting" → normal playback
- Play channel with no files → verify "Loading" → "Downloading" → playback
- Play channel with some files → verify immediate playback

### Automated Testing
- Mock connectivity states → verify correct message returned
- Mock channel states → verify priority logic
- Mock download progress → verify progress display
- Test message persistence (minimum display time)

### User Testing
- Gather feedback on message clarity
- Test with users unfamiliar with p3a
- Iterate on message wording based on confusion points
