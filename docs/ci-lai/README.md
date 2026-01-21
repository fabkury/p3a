# Ci/LAi Management Documentation

This directory contains comprehensive analysis of p3a's Channel Index (Ci) and Locally Available index (LAi) management system.

## Document Overview

### 📊 [analysis.md](./analysis.md)
**Comprehensive technical analysis** of the current implementation, including:
- Data structure definitions
- Architectural components
- Detailed evaluation of each proposed behavior
- Identification of critical bugs and race conditions
- Edge cases and failure scenarios
- Strengths and weaknesses
- Alternative approaches and design patterns
- Code quality assessment

**Audience:** Engineers implementing fixes, architects reviewing design  
**Length:** ~22,000 words  
**Key Finding:** 2 critical bugs must be fixed before production

---

### ✅ [behavior-compliance.md](./behavior-compliance.md)
**Compliance audit** against the proposed behaviors from the problem statement:
- Line-by-line evaluation of each requirement
- ✅ Compliant / ❌ Non-Compliant / ⚠️ Partial status for each behavior
- Evidence from source code
- Specific issues identified
- Scorecard and risk assessment

**Audience:** Product managers, QA, stakeholders  
**Length:** ~12,000 words  
**Key Finding:** 62.5% compliance (5/8 behaviors), 2 critical issues

---

### 🔧 [implementation-checklist.md](./implementation-checklist.md)
**Actionable fix guide** with concrete code changes needed:
- Step-by-step fixes for critical issues
- Code snippets showing before/after
- Testing checklist (unit, integration, system)
- Performance optimization recommendations
- Configuration tuning guidelines
- Sign-off criteria

**Audience:** Developers implementing fixes  
**Length:** ~9,000 words  
**Estimated Effort:** 3-5 days for critical fixes

---

## Quick Start

**If you're new to this issue:**
1. Read [behavior-compliance.md](./behavior-compliance.md) for the high-level status
2. Review the "Critical Fixes Required" section in [implementation-checklist.md](./implementation-checklist.md)
3. Dive into [analysis.md](./analysis.md) for technical details

**If you're implementing fixes:**
1. Start with [implementation-checklist.md](./implementation-checklist.md)
2. Reference [analysis.md](./analysis.md) for context and design decisions
3. Use [behavior-compliance.md](./behavior-compliance.md) to verify compliance after fixes

**If you're reviewing/approving:**
1. Check the scorecard in [behavior-compliance.md](./behavior-compliance.md)
2. Review "Recommendations" section in [analysis.md](./analysis.md)
3. Verify sign-off checklist in [implementation-checklist.md](./implementation-checklist.md)

---

## Critical Issues Summary

### Issue #1: Incomplete Ci Eviction (CRITICAL)
**Status:** ❌ Data Corruption Risk  
**Location:** `components/channel_manager/makapix_channel_refresh.c:711-776`  
**Problem:** `evict_excess_artworks()` only deletes files, not Ci entries. Ci can grow unbounded.  
**Impact:** Violates 1,024 entry limit, memory exhaustion, LAi indices become invalid  
**Fix Difficulty:** Medium (2 days)

### Issue #2: State Invariant Violation (CRITICAL)
**Status:** ❌ Invalid States Possible  
**Location:** `components/channel_manager/makapix_channel_refresh.c:920-1002`  
**Problem:** New entries added before eviction. Ci temporarily exceeds 1,024 entries.  
**Impact:** Race conditions, concurrent access to invalid state  
**Fix Difficulty:** Easy (0.5 days)

### Issue #3: Download vs. Eviction Race (HIGH)
**Status:** ⚠️ Race Condition  
**Location:** Multiple files (download_manager.c, makapix_channel_refresh.c)  
**Problem:** Download can target Ci entry that gets evicted mid-download  
**Impact:** Crashes, corrupted LAi, wasted bandwidth  
**Fix Difficulty:** Medium (1-2 days)

---

## Key Terminology

| Term | Definition | File Location |
|------|------------|---------------|
| **Ci** | Channel Index - array of all known artworks (max 1,024) | `channel_cache.h:78` |
| **LAi** | Locally Available index - subset of Ci that are downloaded | `channel_cache.h:81` |
| **bi** | Batch of entries from server query (typically 32) | `makapix_channel_refresh.c:850` |
| **Fill-Availability-Holes** | Download manager task that fills gaps (Ci - LAi) | `download_manager.c:386` |
| **TARGET_COUNT** | Maximum Ci entries (1,024) | `makapix_channel_refresh.c:855` |

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                     Refresh Task (FreeRTOS)                 │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌─────────┐ │
│  │ Query bi │──>│Update Ci │──>│ Eviction │──>│ Signal  │ │
│  └──────────┘   └──────────┘   └──────────┘   └─────────┘ │
│       │                                              │      │
│       │                                              ▼      │
│       │                                      downloads_needed│
│       │                                              │      │
└───────┼──────────────────────────────────────────────┼──────┘
        │                                              │
        │         ┌────────────────────────────────────┘
        │         │
        ▼         ▼
┌─────────────────────────────────────────────────────────────┐
│                   Download Task (FreeRTOS)                  │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌─────────┐ │
│  │Find Hole │──>│ Download │──>│ Update   │──>│ Signal  │ │
│  │ (Ci-LAi) │   │   File   │   │   LAi    │   │  Next   │ │
│  └──────────┘   └──────────┘   └──────────┘   └─────────┘ │
└─────────────────────────────────────────────────────────────┘
        │                   │
        │                   │
        ▼                   ▼
┌────────────┐      ┌────────────┐
│ channel_   │<────>│   Vault    │
│ cache_t    │      │  Storage   │
│ (Ci + LAi) │      │  (Files)   │
└────────────┘      └────────────┘
     Mutex              SD Card
```

---

## Testing Status

| Test Type | Status | Coverage |
|-----------|--------|----------|
| Unit Tests | ❌ None | 0% |
| Integration Tests | ❌ None | 0% |
| System Tests | ⚠️ Manual only | Unknown |
| Performance Tests | ❌ None | 0% |

**Recommendation:** Add tests from [implementation-checklist.md](./implementation-checklist.md) before fixing bugs.

---

## Related Documentation

- [../INFRASTRUCTURE.md](../INFRASTRUCTURE.md) - Overall system architecture
- [../ROADMAP.md](../ROADMAP.md) - Future development plans
- `components/channel_manager/include/channel_cache.h` - API documentation
- `components/channel_manager/include/makapix_channel_impl.h` - Internal structures

---

## Change History

| Date | Author | Changes |
|------|--------|---------|
| 2026-01-20 | Code Analysis Agent | Initial analysis and documentation |

---

## Contact

For questions about this analysis:
- File an issue in the p3a repository
- Reference issue: "Cleanup Ci/LAi management logic"
- Tag: `component:channel_manager`

---

*Last Updated: 2026-01-20*  
*Document Set Version: 1.0*  
*Status: Draft - Awaiting Review*
