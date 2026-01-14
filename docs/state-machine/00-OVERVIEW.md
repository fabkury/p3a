# p3a State Machine Refactoring — Overview

**Document Version:** 2.0  
**Date:** January 2026  
**Status:** Final Specification

---

## 1. Executive Summary

This document series specifies a comprehensive refactoring of p3a's state management apparatus. The goal is to create a clean, rational, and efficient system for:

1. **Connectivity State Tracking** — Hierarchical state machine for WiFi → Internet → Registration → MQTT
2. **Locally Available Index (LAi)** — Per-channel array of locally available artworks for efficient playback
3. **Play Scheduler Behavior** — Non-blocking playback that works immediately with what's available
4. **Background Operations** — Channel refresh and file downloads that never block animation playback
5. **User Communication** — Clear, informative messages about the current system state
6. **Robustness** — Graceful handling of LAi inconsistencies, corrupted files, and edge cases

---

## 2. Design Principles

### 2.1 Immediate Playback
> "p3a always begins playing, immediately or nearly so, what it already has."

- Playback should start within milliseconds of a scheduler command
- Channel refreshes and downloads happen in the background
- The user sees content immediately, not loading spinners

### 2.2 Efficient Availability Checking
> "Random picks should never fail due to unavailable artworks."

- The Play Scheduler only "sees" locally available artworks (LAi)
- No runtime file existence checks during pick operations
- LAi is maintained incrementally as files are downloaded/evicted

### 2.3 Cascading State Awareness
> "If there's no WiFi, don't tell the user there's no MQTT."

- States are hierarchical: WiFi → Internet → Registration → MQTT
- Display the most fundamental missing dependency
- Avoid confusing users with downstream symptoms

### 2.4 Lightweight Web UI
> "Opening the Web UI should not trigger heavy file operations."

- All UI data comes from in-memory state
- No file scanning or counting on UI load
- Statistics are cached and refreshed periodically in background

### 2.5 Robustness Over Tracking
> "Be robust to failures rather than trying to track every possible state."

- Don't track files across channels; instead handle LAi load failures gracefully
- If a file in LAi fails to load, evict it and handle appropriately
- Use Load Tracker Files (LTF) to prevent infinite re-download loops of corrupted files

### 2.6 Event-Driven Architecture
> "Use message queues instead of polling."

- Tasks communicate via FreeRTOS queues, not polling loops
- Clearer control flow and reduced CPU usage
- Easier to abort operations and handle interruptions

---

## 3. Current Architecture (Before Refactoring)

### 3.1 State Management Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `app_state` | `components/app_state/` | Simple 3-state machine (READY/PROCESSING/ERROR) |
| `p3a_state` | `components/p3a_core/` | Global states (ANIMATION_PLAYBACK/PROVISIONING/OTA/PICO8) |
| `makapix_channel_events` | `components/channel_manager/` | FreeRTOS event group for connectivity signals |
| `play_scheduler` | `components/play_scheduler/` | Playback engine with channel cache loading |
| `download_manager` | `components/channel_manager/` | Background file download task |

### 3.2 Current Pain Points

1. **No LAi** — Availability checked on-the-fly via `file_exists()` during picks
2. **Flat connectivity states** — WiFi/MQTT tracked as independent event bits
3. **Blocking on refresh** — Download manager waits for full refresh completion
4. **Heavy Web UI queries** — `/channels/stats` scans files on each call
5. **No atomic Ci+LAi persistence** — Risk of sync issues between index and availability
6. **Polling-based architecture** — Tasks poll for work instead of event-driven

---

## 4. Proposed Architecture (After Refactoring)

### 4.1 New Components

| Component | Purpose |
|-----------|---------|
| `connectivity_state` | Hierarchical state machine for network dependencies |
| `channel_cache` | Per-channel Ci+LAi management and persistence |
| `load_tracker` | LTF system to prevent infinite re-download loops |

### 4.2 Modified Components

| Component | Changes |
|-----------|---------|
| `play_scheduler` | Picks from LAi instead of Ci; no file_exists() during picks; robust to LAi failures |
| `download_manager` | Event-driven; starts downloading as soon as entries exist in Ci; respects LTF |
| `p3a_state` | Integrates connectivity hierarchy for user messages |
| `http_api` | Returns cached stats; no file scanning |

---

## 5. Document Structure

This specification is split into the following documents:

| Document | Content |
|----------|---------|
| [01-CONNECTIVITY-STATE.md](01-CONNECTIVITY-STATE.md) | Cascading WiFi/Internet/Registration/MQTT state machine |
| [02-LOCALLY-AVAILABLE-INDEX.md](02-LOCALLY-AVAILABLE-INDEX.md) | LAi concept, data structures, persistence, LTF mechanism |
| [03-PLAY-SCHEDULER-BEHAVIOR.md](03-PLAY-SCHEDULER-BEHAVIOR.md) | Scheduler commands, immediate playback, background work |
| [05-MIGRATION-PLAN.md](05-MIGRATION-PLAN.md) | Step-by-step implementation approach |
| [06-PAIN-POINTS.md](06-PAIN-POINTS.md) | Current inefficiencies and architectural weaknesses |

---

## 6. Glossary

| Term | Definition |
|------|------------|
| **Ci** | Channel index — array of up to 1,024 artworks known to exist for channel `i` |
| **LAi** | Locally Available index — subset of Ci containing only downloaded artworks |
| **SC** | Scheduler Command — instruction to play specific channels |
| **PS** | Play Scheduler — the playback engine |
| **NAE** | New Artwork Event — out-of-band notification of new content |
| **SWRR** | Smooth Weighted Round Robin — fair channel scheduling algorithm |
| **LTF** | Load Tracker File — marker file tracking failed load attempts to prevent infinite re-download loops |

---

## 7. Success Criteria

The refactoring is successful when:

1. ✅ Playback starts within 100ms of scheduler command (when LAi is non-empty)
2. ✅ Random picks never scan files or retry due to unavailability
3. ✅ User sees appropriate message for their connectivity situation
4. ✅ Web UI loads instantly with cached statistics
5. ✅ Downloads start before channel refresh completes
6. ✅ LAi and Ci are always in sync (atomic persistence)
7. ✅ Corrupted files don't cause infinite re-download loops
8. ✅ All files deleted from SD card doesn't crash or hang the device

---

*Next: [01-CONNECTIVITY-STATE.md](01-CONNECTIVITY-STATE.md)*
