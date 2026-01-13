# State Machine Refactoring - Executive Summary

## Purpose

This document collection represents the comprehensive planning phase for refactoring p3a's state control apparatus. The goal is to clarify and rationalize how p3a handles global state, makes decisions about what to do next, keeps users informed, and plays channels and artworks.

## The Problem

p3a has accumulated technical debt in several key areas:

1. **State Control**: Global state management is fragmented and unclear
2. **Playback Decision-Making**: The logic for what to play next is scattered
3. **User Information**: Users don't always know what's happening (downloading, refreshing, offline, etc.)
4. **Channel Refreshes**: Background operations don't cleanly integrate with playback
5. **File Downloads**: Download orchestration blocks or conflicts with playback
6. **Availability Tracking**: No efficient way to track which files are locally available (LAi)

## Key Design Requirements

### Connectivity States (Cascading)
```
WI-FI/NO WI-FI
  └─ INTERNET/NO INTERNET
      └─ MPX REGISTRATION/NO MPX REGISTRATION
          └─ MQTT/NO MQTT
```

### Behavior Modes

**Offline Mode (MQTT unavailable):**
- Play all locally available files
- No channel index refreshes
- No artwork downloads
- No MQTT interactions or view tracking

**Online Mode (MQTT available):**
- All offline mode functionality
- **Plus background operations:**
  - Refresh channel indices
  - Download missing artworks
  - Send view tracking events
- **Crucially: Background operations NEVER block playback**

### Play Scheduler Commands (SC)

When a new scheduler command arrives:
1. Play immediately from what's already available (LAi)
2. Begin refreshing channel indices (if MQTT available)
3. Scan for file availability holes and download in background
4. If all LAi arrays are empty: show informative messages
5. When first file downloads: call `next()` to start playback

### Locally Available Index (LAi)

**What it is:**
- For each channel, LAi is a subset of the channel index (Ci) containing ONLY locally available artworks
- Persisted to SD card alongside Ci in the same file (atomic operations)
- Updated when files are downloaded or evicted

**Why it's needed:**
- Enables efficient random walks through available artworks
- Eliminates "skip unavailable artwork" retry loops
- Play Scheduler only sees/considers artworks in LAi, never raw Ci
- Empty LAi = channel has no playable content

**Operations:**
- Add to LAi: When file download completes
- Remove from LAi: When file is evicted OR fails to load
- Persist: With Ci in single atomic write

## Document Structure

1. **00-EXECUTIVE-SUMMARY.md** (this file) - Overview
2. **01-CONNECTIVITY-STATE-SPEC.md** - Wi-Fi/Internet/MQTT state tracking
3. **02-CHANNEL-INDEX-SPEC.md** - LAi architecture and Ci+LAi persistence
4. **03-PLAYBACK-ORCHESTRATION.md** - Play Scheduler behavior and SC handling
5. **04-DOWNLOAD-COORDINATION.md** - Background downloading without blocking
6. **05-USER-FEEDBACK.md** - Informative messages and state display
7. **06-IMPLEMENTATION-PLAN.md** - Code changes, migration strategy, pain points
8. **07-OPEN-QUESTIONS.md** - Unresolved design decisions and alternatives

## Current Architecture Assessment

### What Works Well
- **p3a_state.c**: Centralized global state machine is a good foundation
- **play_scheduler.c**: On-demand streaming generator with availability masking
- **download_manager.c**: Decoupled download state and round-robin logic
- **Channel interface**: Clean abstraction for SD card vs Makapix channels

### Pain Points
- **No LAi**: Channels include unavailable files, requiring skip logic
- **Cache format**: Ci and LAi need to be in same file but aren't currently
- **Refresh blocking**: Unclear when refreshes should interrupt playback
- **Status reporting**: Web UI queries can trigger heavy file operations
- **Download triggers**: Not clear when download manager should wake up
- **MQTT state**: Not clearly integrated into play/don't play decisions

## Success Criteria

After refactoring, p3a should:

1. ✅ Have clear source-of-truth for Wi-Fi/Internet/MQTT/Registration state
2. ✅ Never block playback for background operations (refresh/download)
3. ✅ Only attempt refresh/download when MQTT is connected
4. ✅ Show appropriate messages when no artworks are available
5. ✅ Efficiently pick random artworks without retries (using LAi)
6. ✅ Handle scheduler commands cleanly (interrupt refresh, allow in-flight downloads)
7. ✅ Persist Ci+LAi atomically to prevent sync issues
8. ✅ Populate web UI with lightweight in-memory data (no heavy file ops)

## Timeline Estimate

- **Planning Phase**: 1-2 days (this document set)
- **Review & Iteration**: 1-2 days (refine based on feedback)
- **Implementation Phase**: 5-7 days
  - LAi persistence refactor: 2 days
  - State tracking integration: 1 day
  - Download coordination: 1-2 days
  - Testing & validation: 1-2 days

Total: ~2-3 weeks with proper planning and testing.
