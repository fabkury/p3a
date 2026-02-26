# First-Principles Integration Plan: v1 + v2

This document harmonizes the original first-principles analysis (v1) with a fresh codebase assessment (v2) into a unified architectural improvement plan.

## Relationship Between v1 and v2

| v1 Topic | v2 Topic | Relationship |
|----------|----------|--------------|
| State Machine Single Source of Truth | Unified State Hierarchy | **v2 extends** with concrete hierarchy design |
| Central Event Bus | Event-Driven Architecture | **v2 extends** with event taxonomy and implementation details |
| Formalize Layering and Dependencies | Service Layer Boundaries | **v2 extends** with content pipeline specifics |
| Resource Ownership | (covered in Service Layer) | **v1 is authoritative** for SDIO arbitration |
| Separate Domain from Transport | (implicit in Content Pipeline) | **v1 is authoritative** for Makapix separation |
| Testable Core | (implicit in decomposition) | **v1 is authoritative** for host-side testing |
| Clarify UI Transitions | (merged into State Hierarchy) | **v2 absorbs** into unified state machine |
| Deduplicate Includes/Config | Include Dependency Reduction | **v2 extends** with specific targets |
| Encapsulate Display Renderer Globals | Animation Player Decomposition | **v2 extends** with broader scope |
| Make Animation Player State Private | Animation Player Decomposition | **v2 extends** with decomposition strategy |
| Reduce Weak Symbol Coupling | Handler Registration Pattern | **v2 refines** with concrete pattern |
| Standardize Error Handling | (incorporated in all v2 docs) | **v1 is authoritative** |
| — | Content Pipeline Refactoring | **v2 adds** new perspective |
| — | Task Consolidation | **v2 adds** new perspective |
| — | Channel Manager Reorganization | **v2 adds** new perspective |
| — | Deprecated API Removal | **v2 adds** new perspective |

## Unified Improvement Sequence

The improvements should be executed in phases that respect dependencies and minimize risk.

### Phase 1: Foundation (Prerequisites)

These changes enable all subsequent work:

1. **Standardize Error Handling** (v1)
   - Define boot phases and failure policies
   - Create `p3a_boot_check()` helper
   - *Why first*: Establishes consistent patterns for all subsequent changes

2. **Encapsulate Globals** (v1 + v2)
   - `display_renderer_state_t` struct (v1)
   - Animation player state privatization (v1 + v2)
   - *Why second*: Reduces coupling before restructuring

3. **Handler Registration Pattern** (v1 + v2)
   - Replace weak symbols with explicit registration
   - *Why third*: Makes dependencies explicit before adding event bus

### Phase 2: Core Architecture

These are the major architectural improvements:

4. **Unified State Hierarchy** (v1 + v2)
   - Consolidate `p3a_state`, `connectivity_state`, `makapix_state`, `app_state`
   - Single hierarchical state machine
   - *Dependencies*: Phase 1 complete

5. **Event-Driven Architecture** (v1 + v2)
   - Create `event_bus` component
   - Migrate cross-module calls to events
   - *Dependencies*: State hierarchy defined

6. **Service Layer Boundaries** (v1 + v2)
   - Define `content_service`, `playback_service`, `connectivity_service`
   - Enforce layering in CMake
   - *Dependencies*: Event bus available

### Phase 3: Domain Refinement

These refine specific subsystems:

7. **Content Pipeline Refactoring** (v2)
   - Separate fetch → cache → decode → render → display
   - *Dependencies*: Service layer defined

8. **Separate Domain from Transport** (v1)
   - Extract `makapix_domain` from `makapix_transport`
   - *Dependencies*: Content pipeline clear

9. **Animation Player Decomposition** (v2)
   - Split into ContentLoader, FrameRenderer, PlaybackController
   - *Dependencies*: Content pipeline and service layer ready

### Phase 4: Cleanup and Optimization

These can proceed in parallel after Phase 3:

10. **Channel Manager Reorganization** (v2)
    - Consolidate 15 headers into logical groups
    - *Dependencies*: Service layer stable

11. **Task Consolidation** (v2)
    - Audit and reduce FreeRTOS task count
    - *Dependencies*: Event bus handling cross-task communication

12. **Include Dependency Reduction** (v1 + v2)
    - Remove circular dependencies
    - Reduce `animation_player.c` includes from 24+ to ~10
    - *Dependencies*: Decomposition complete

13. **Deprecated API Removal** (v2)
    - Remove `animation_player_cycle_animation()` and similar
    - *Dependencies*: All callers migrated

14. **Testable Core** (v1)
    - Extract pure logic for host-side testing
    - *Dependencies*: Decomposition complete

## Architectural Principles (v1 + v2 Combined)

These principles guide all changes:

1. **Single State Authority**: All mode/UI/playback decisions flow through `p3a_state`
2. **Event-Driven Communication**: Components emit events, don't call each other directly
3. **Unidirectional Dependencies**: HAL → Infra → Service → App, never backwards
4. **Explicit Ownership**: Shared resources have one owner (SDIO, frame buffers, etc.)
5. **Domain/Transport Separation**: Business logic independent of network protocols
6. **Testable Core**: Deterministic logic runnable without ESP-IDF

## Risk Mitigation Strategy

| Risk | Mitigation |
|------|------------|
| Regressions during refactor | One flow at a time; keep fallback paths |
| Event storms | Backpressure, coalescing, queue limits |
| Deadlocks from ownership changes | Timeouts, resource logging |
| Build complexity for testing | Separate `host_tests` CMake target |
| Breaking existing features | Feature flags for gradual rollout |

## Success Metrics

After completing all phases:

- [ ] One state machine explains all device behavior
- [ ] Any feature addition requires only emitting/consuming events
- [ ] No circular dependencies between components
- [ ] Core logic (scheduler, channel selection) testable on host
- [ ] Boot sequence is phased with clear failure handling
- [ ] `animation_player.c` is < 300 lines (decomposed into focused modules)
- [ ] Task count reduced by ~30%

## Files in This Directory

### High-Level Improvements

| File | Purpose |
|------|---------|
| `01-unified-state-hierarchy.md` | Concrete design for merged state machine |
| `02-event-driven-architecture.md` | Event taxonomy and implementation |
| `03-content-pipeline-refactoring.md` | Fetch→decode→render separation |
| `04-service-layer-boundaries.md` | Service definitions and layering rules |
| `05-task-consolidation.md` | FreeRTOS task audit and reduction |

### Low-Level Improvements

| File | Purpose |
|------|---------|
| `01-animation-player-decomposition.md` | Breaking up the monolith |
| `02-channel-manager-reorganization.md` | Header consolidation |
| `03-deprecated-api-removal.md` | Cleanup of legacy interfaces |
| `04-include-dependency-reduction.md` | Specific targets for decoupling |
| `05-handler-registration-pattern.md` | Replacing weak symbols |
