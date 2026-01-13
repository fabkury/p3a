# State Machine Refactoring - Document Index

## Welcome

This directory contains comprehensive planning documentation for refactoring p3a's state control apparatus. This planning phase addresses technical debt that has accumulated around:

- State control and decision-making
- Artwork playback orchestration  
- Play Scheduler and channel management
- Channel refresh operations
- File download coordination
- User feedback and status messaging

## Reading Order

For a complete understanding, read the documents in this order:

### 1. Start Here: Executive Summary
**File**: `00-EXECUTIVE-SUMMARY.md`

**What it covers**:
- Problem statement and motivation
- Key design requirements
- Document structure overview
- Current architecture assessment
- Success criteria

**Time to read**: 10 minutes

---

### 2. Connectivity State Specification
**File**: `01-CONNECTIVITY-STATE-SPEC.md`

**What it covers**:
- Cascading connectivity states (Wi-Fi → Internet → Registration → MQTT)
- Online/offline mode determination
- State tracking architecture
- User messaging priorities
- Integration with existing components

**Key innovation**: Clear hierarchical state model prevents confusing error messages.

**Time to read**: 15 minutes

---

### 3. Channel Index Specification
**File**: `02-CHANNEL-INDEX-SPEC.md`

**What it covers**:
- Problem with current channel indices (mixing available and unavailable files)
- LAi (Locally Available Index) architecture
- Ci + LAi persistence strategy
- LAi operations (add, remove, check)
- Memory and performance analysis

**Key innovation**: LAi enables O(1) picks without "skip unavailable" retry logic.

**Time to read**: 20 minutes

---

### 4. Playback Orchestration
**File**: `03-PLAYBACK-ORCHESTRATION.md`

**What it covers**:
- Scheduler Command (SC) lifecycle
- Background refresh behavior (non-blocking)
- Play Scheduler `next()` algorithm with LAi
- Handling empty LAi (cold start)
- Informative message display logic

**Key innovation**: Playback never blocks for background operations.

**Time to read**: 25 minutes

---

### 5. Download Coordination
**File**: `04-DOWNLOAD-COORDINATION.md`

**What it covers**:
- Download Manager architecture (decoupled from Play Scheduler)
- Round-robin file downloading
- Finding "availability holes" in channel indices
- Download completion hooks (adding to LAi)
- File sharing across channels (deduplication)

**Key innovation**: One download at a time, fills LAi incrementally, triggers playback on first completion.

**Time to read**: 25 minutes

---

### 6. User Feedback
**File**: `05-USER-FEEDBACK.md`

**What it covers**:
- Message hierarchy and priorities
- Message templates for all states
- Display integration
- Web UI status endpoint
- Debug overlays and logging

**Key innovation**: Priority-based messages ensure users always understand what's happening.

**Time to read**: 20 minutes

---

### 7. Implementation Plan
**File**: `06-IMPLEMENTATION-PLAN.md`

**What it covers**:
- File-by-file code changes for each phase
- Migration strategy and backward compatibility
- Pain points and mitigation strategies
- Testing strategy
- Rollout plan and success metrics

**This is the "how"**: Concrete steps to implement the designs above.

**Time to read**: 30 minutes

---

### 8. Open Questions
**File**: `07-OPEN-QUESTIONS.md`

**What it covers**:
- Unresolved design decisions
- Analysis of options for each question
- Recommendations with rationale
- Decision log template

**Use this for**: Discussion and final decision-making before implementation.

**Time to read**: 25 minutes

---

## Total Reading Time

- **Quick overview**: 30 minutes (Executive Summary + Implementation Plan summary)
- **Full understanding**: 2.5-3 hours (all documents)
- **Deep dive with notes**: 4-5 hours

## Document Statistics

| Document | Lines | Words | Key Concepts |
|----------|-------|-------|--------------|
| 00-EXECUTIVE-SUMMARY.md | 200 | 5,000 | Problem, requirements, success criteria |
| 01-CONNECTIVITY-STATE-SPEC.md | 300 | 7,800 | Cascading states, online mode |
| 02-CHANNEL-INDEX-SPEC.md | 450 | 11,500 | LAi architecture, Ci+LAi persistence |
| 03-PLAYBACK-ORCHESTRATION.md | 550 | 14,900 | SC lifecycle, non-blocking refresh |
| 04-DOWNLOAD-COORDINATION.md | 600 | 16,300 | Round-robin, hole filling, deduplication |
| 05-USER-FEEDBACK.md | 600 | 16,200 | Message priorities, display integration |
| 06-IMPLEMENTATION-PLAN.md | 850 | 24,000 | Code changes, migration, pain points |
| 07-OPEN-QUESTIONS.md | 700 | 19,500 | Design decisions, recommendations |
| **Total** | **4,250** | **115,200** | **Comprehensive planning** |

## Key Terms Glossary

### LAi (Locally Available Index)
A subset of a channel's index (Ci) containing only artworks that are downloaded and playable locally. Eliminates retry loops when picking artworks.

**Example**: Channel has 1000 entries, but only 50 are downloaded. LAi contains those 50 indices.

### Ci (Channel Index)
The complete channel index containing all artworks (up to 1,024), both available and unavailable locally.

### Scheduler Command (SC)
A command that tells the Play Scheduler which channels to play and how. Examples: play "promoted", play 3 channels simultaneously, play user's channel.

### Online Mode
Boolean state indicating p3a can perform background operations (refresh, download, view tracking). True only when Wi-Fi, Internet, Registration, and MQTT are all available.

### Offline Mode
State when MQTT is not available. p3a plays only locally available files, no background operations.

### SWRR (Smooth Weighted Round Robin)
Algorithm for fairly distributing picks across multiple channels based on configurable weights.

### File Availability Hole
An entry in a channel index (Ci) that is not downloaded locally (not in LAi). Download Manager scans for holes and fills them.

### Cascading States
Hierarchical connectivity states where child states depend on parent states. If Wi-Fi is down, all child states (Internet, Registration, MQTT) are implicitly unavailable.

## Visual Architecture Diagrams

### High-Level State Flow

```
┌─────────────────────────────────────────────────────────────┐
│                         User Action                         │
│              (Touch, MQTT command, Web UI)                  │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
         ┌─────────────────────────────┐
         │   Scheduler Command (SC)    │
         │   ("Play channel X")        │
         └──────────────┬──────────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼
    ┌───────┐    ┌──────────┐    ┌──────────┐
    │ Stop  │    │ Interrupt│    │  Allow   │
    │ View  │    │ Refresh  │    │ Download │
    │Track  │    │  (Abort) │    │(Complete)│
    └───┬───┘    └────┬─────┘    └────┬─────┘
        │             │               │
        └─────────────┼───────────────┘
                      │
                      ▼
            ┌──────────────────┐
            │   Load LAi       │
            │  (Cache Files)   │
            └─────────┬────────┘
                      │
        ┌─────────────┴─────────────┐
        │                           │
        ▼                           ▼
   ┌─────────┐               ┌──────────┐
   │ LAi > 0 │               │ LAi = 0  │
   │ Play!   │               │ Show Msg │
   └─────────┘               └────┬─────┘
                                  │
                    ┌─────────────┴─────────────┐
                    │                           │
                    ▼                           ▼
          ┌──────────────┐            ┌─────────────┐
          │   Refresh    │            │  Download   │
          │(Background)  │            │(Background) │
          └──────┬───────┘            └──────┬──────┘
                 │                           │
                 └────────────┬──────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  First Download  │
                    │   Complete →     │
                    │  Trigger Play    │
                    └──────────────────┘
```

### Connectivity State Cascade

```
┌──────────────────────────────────────────────────────────┐
│                        Wi-Fi                             │
│                    ┌──────────┐                          │
│                    │Connected │                          │
│                    └─────┬────┘                          │
│                          │                               │
│    ┌─────────────────────┴─────────────────────┐        │
│    │              Internet                     │        │
│    │          ┌──────────┐                     │        │
│    │          │Available │                     │        │
│    │          └─────┬────┘                     │        │
│    │                │                          │        │
│    │    ┌───────────┴───────────┐             │        │
│    │    │    MPX Registration   │             │        │
│    │    │    ┌──────────┐       │             │        │
│    │    │    │Registered│       │             │        │
│    │    │    └─────┬────┘       │             │        │
│    │    │          │            │             │        │
│    │    │    ┌─────┴──────┐    │             │        │
│    │    │    │    MQTT    │    │             │        │
│    │    │    │ ┌────────┐ │    │             │        │
│    │    │    │ │Connected│ │    │             │        │
│    │    │    │ └────┬───┘ │    │             │        │
│    │    │    │      │     │    │             │        │
│    │    │    │  ┌───▼────┐│    │             │        │
│    │    │    │  │ ONLINE ││    │             │        │
│    │    │    │  │  MODE  ││    │             │        │
│    │    │    │  └────────┘│    │             │        │
│    │    │    └────────────┘    │             │        │
│    │    └────────────────────────┘             │        │
│    └─────────────────────────────────────────┘        │
└──────────────────────────────────────────────────────────┘

If any parent level fails, all child levels become unavailable.
User sees the most fundamental problem (topmost failure).
```

### LAi Architecture

```
Channel Cache File: /sdcard/p3a/channel/promoted.bin

┌────────────────────────────────────────────────────────┐
│                       HEADER                           │
│  ┌──────────────────────────────────────────────────┐ │
│  │ entry_count: 1024                                │ │
│  │ lai_count:   156                                 │ │
│  └──────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────┤
│                    Ci SECTION                          │
│  ┌──────────────────────────────────────────────────┐ │
│  │ entry[0]:    post_id=12345, uuid=abc, ext=.webp  │ │
│  │ entry[1]:    post_id=12346, uuid=def, ext=.gif   │ │
│  │ entry[2]:    post_id=12347, uuid=ghi, ext=.webp  │ │
│  │ ...                                              │ │
│  │ entry[1023]: post_id=23456, uuid=xyz, ext=.png   │ │
│  └──────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────┤
│                   LAi SECTION                          │
│  ┌──────────────────────────────────────────────────┐ │
│  │ lai_indices[0]:   0    (→ entry[0] available)   │ │
│  │ lai_indices[1]:   5    (→ entry[5] available)   │ │
│  │ lai_indices[2]:   12   (→ entry[12] available)  │ │
│  │ ...                                              │ │
│  │ lai_indices[155]: 892  (→ entry[892] available) │ │
│  └──────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────┘

Play Scheduler picks: idx = random() % 156
                     ci_idx = lai_indices[idx]
                     entry = Ci[ci_idx]
                     → Guaranteed available!
```

## FAQ

### Q: Why create LAi instead of just checking file existence during picks?

**A**: Checking file existence (stat()) is expensive, especially for random picks. LAi moves this cost to download time (one-time) instead of playback time (frequent).

### Q: What if LAi becomes stale (files deleted externally)?

**A**: System self-heals. When playback detects missing file, it removes the entry from LAi. Download manager also checks existence before downloading.

### Q: How does this handle SD card swap?

**A**: On SD mount, rebuild LAi from scratch by scanning filesystem. Expensive but only happens on major events.

### Q: Can we have multiple downloads at once?

**A**: Currently no (one at a time to avoid SD contention). Future enhancement could allow 2-3 concurrent downloads with careful coordination.

### Q: What about OTA updates during download?

**A**: Download manager checks `animation_player_is_sd_paused()` before operations. OTA can pause downloads cleanly.

## Contributing

When updating these documents:

1. **Maintain consistency**: Update all affected documents when changing a design
2. **Version changes**: Note significant changes in git commit messages
3. **Cross-reference**: Add links between related sections in different documents
4. **Keep diagrams current**: Update ASCII diagrams when architecture changes
5. **Test examples**: Ensure code examples compile and follow current style

## License

These documents are part of the p3a project and share the same license (Apache 2.0). See `LICENSE` file in repository root.

## Authors

- Primary author: GitHub Copilot (AI planning assistant)
- Reviewer: p3a maintainers
- Based on requirements from: fabkury

## Last Updated

- Document set created: 2026-01-13
- Last significant update: 2026-01-13
- Next review: Before Phase 1 implementation begins

---

**Ready to start?** Begin with `00-EXECUTIVE-SUMMARY.md` and proceed through the documents in order. Happy planning! 🚀
