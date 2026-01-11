# Play Scheduler — Specification

**Streaming Generator with History, Lookahead Batching, and New Artwork Events (NAE)**

> This document is the original specification provided by the product owner.
> See `IMPLEMENTATION_PLAN.md` for implementation details and decisions.

---

## 0. Overview and Design Intent

The **Play Scheduler** is a deterministic playback engine that selects artworks from multiple followed channels for presentation on a player device.

Key design principles:

* The play queue is **virtual**, not materialized in full.
* Playback is driven by a **streaming generator** with bounded memory.
* The system balances:

  * fairness across channels,
  * exploration within channels,
  * responsiveness to newly published artworks.
* The design is optimized for embedded environments with:

  * SD + WiFi bus contention,
  * ample PSRAM,
  * deterministic and reversible PRNGs.

The scheduler exposes a minimal interface:

* `next()`
* `prev()` (limited to history)
* `peek(n)` (non-destructive, read-only)

---

## 1. Core Concepts and Definitions

### 1.1 Channels

* The device follows **N channels**, where `1 ≤ N ≤ 64`.
* Each channel represents an arbitrarily large remote stream of artworks.

### 1.2 Local Channel Cache (Ei)

For each channel `i`:

* A local SD binary file `Ei` stores up to **K** most recent artworks.
* K is configurable (typical range: 8–8192).
* Each record:

  * fixed size (~80 bytes),
  * includes:

    * `id` (globally unique artwork ID),
    * `ts` (creation timestamp),
    * payload.
* Records must be accessible efficiently from **newest → older**.

### 1.3 Snapshot Semantics

The scheduler operates on a **logical snapshot** defined by:

* the set of followed channels,
* the contents of each `Ei`,
* exposure mode and parameters,
* NAE configuration and current pool state.

Any **material change** resets the scheduler:

* follow/unfollow channel,
* cache refresh or rewrite,
* exposure mode or parameter change,
* NAE enable/disable or configuration change.

---

## 2. Public Interface

### 2.1 Operations

* `next() -> ArtworkRecord`
  Advances playback and returns the next artwork.

* `prev() -> ArtworkRecord | none`
  Walks backward through history only.

* `peek(n) -> list[ArtworkRecord]`
  Returns up to `n` upcoming items from lookahead **without generating new items**.

### 2.2 Buffers

* **History buffer (H)**

  * Default: `H = 32`, configurable.
  * Stores the last H *committed* items.
  * `prev()` and forward traversal through history never mutate generator state.

* **Lookahead buffer (L)**

  * Default: `L = 32`, configurable.
  * Stores future items generated in batches.
  * L is a **minimum fill threshold**, not a hard cap.

---

## 3. History Semantics (Final)

History is **purely navigational**.

* `prev()`:

  * walks backward through available history,
  * does **not** change scheduler state,
  * does **not** invalidate lookahead,
  * does **not** affect future generation.

* After X calls to `prev()`, the next X calls to `next()`:

  * simply walk forward through the same history,
  * without invoking generation logic.

* Once the user advances past the most recent history entry, normal generation resumes.

This design deliberately avoids "tape-recorder state rewind" complexity.

---

## 4. Lookahead Semantics (Final)

* `peek(n)`:

  * **never triggers generation**,
  * simply returns up to `n` items already present in lookahead.

* `next()`:

  * before emitting:

    * checks if `lookahead.size < L`,
    * if so, generates **exactly L new items in one batch** and appends them.
  * consumes one item from lookahead (unless serving from history).

This batching:

* amortizes SD I/O,
* reduces scheduler overhead,
* keeps behavior deterministic.

---

## 5. Exposure Modes (Base Scheduler)

The base scheduler decides **which channel** supplies the next artwork.

Only channels with local data (`Mi > 0`) are considered active.

### 5.1 EqE — Equal Exposure

* For each active channel:

  * `weight[i] = 1`
* Inactive channels:

  * `weight[i] = 0`
* Normalize weights across active channels.

---

### 5.2 MaE — Manual Exposure

* `weight[i] = max(0, manual_weight[i])`
* Inactive channels forced to 0.
* Normalize.

---

### 5.3 PrE — Proportional Exposure with Recency Bias

Inputs per channel (from server):

* `total_count[i]`
* `recent_count[i]`

Defaults (chosen for you):

* **α = 0.35**
* **p_min = 0.02**
* **p_max = 0.40**

Computation:

1. Normalize:

   * `p_total[i] = total_count[i] / Σ total_count`
   * `p_recent[i] = recent_count[i] / Σ recent_count`

     * if Σ recent_count = 0 → all `p_recent[i] = 0`
2. Blend:

   * `p_raw[i] = (1 − α) * p_total[i] + α * p_recent[i]`
3. Clamp:

   * `p_clamped[i] = clamp(p_raw[i], p_min, p_max)`
4. Renormalize:

   * `weight[i] = p_clamped[i] / Σ p_clamped`

Inactive channels always have weight 0.

---

## 6. Channel Scheduling Algorithm

### Smooth Weighted Round Robin (SWRR)

* Convert normalized weights into integers `W[i]`
* Choose `Wsum = 65536`
* Initialize:

  * `credit[i] = 0`

For each scheduling step:

1. `credit[i] += W[i]` for all i
2. Select `j = argmax(credit[i])`

   * tie-break: lowest channel ID
3. Emit channel `j`
4. `credit[j] -= Wsum`

This produces smooth, deterministic exposure matching weights.

---

## 7. Per-Channel Artwork Selection

### 7.1 Pick Modes

#### RecencyPick

* Per-channel cursor starts at newest record.
* Each pick:

  * returns record at cursor,
  * cursor moves toward older,
  * wraps on exhaustion.

Repeat handling:

* On immediate repeat:

  * skip up to **2 records** before accepting repeat.

---

#### RandomPick

* Define `R_eff = min(R, Mi)`
* Sample uniformly from newest `R_eff` records.
* Uses deterministic PRNG stream `PRNG_randompick`.

Repeat handling:

* Up to **5 resampling attempts** before accepting repeat.

---

## 8. Immediate Repeat Avoidance (Global)

If selected artwork `id == last_played_id`:

* apply per-mode retry/skip rules,
* if still repeated, accept repeat.

Repeat avoidance is **best-effort**, never blocking.

---

## 9. New Artwork Events (NAE)

### 9.1 Purpose

NAEs provide **temporary, probabilistic, out-of-band exposure** for newly published artworks.

NAE does **not** affect:

* channel weights,
* SWRR credits,
* base exposure accounting.

---

### 9.2 NAE Pool

* Enabled by user setting.
* Pool size cap: **J = 32**
* Each entry:

  * artwork record,
  * `priority` ∈ (0,1],
  * insertion timestamp (for tie-breaking).

---

### 9.3 Insertion (MQTT-triggered)

Upon receiving notification:

* If artwork ID already exists in pool:

  * **merge** entries:

    * reset priority to **50%**.
* Else:

  * insert with priority = 50%.

If pool exceeds J:

* discard entry with **lowest priority**,
* if tie:

  * discard **oldest insertion first**.

---

### 9.4 Selection Probability

At generation time:

* Let priorities be `p1..pm`
* Compute:

  * `P = min(1, Σ pi)`

Using deterministic PRNG `PRNG_nae`:

* if `r < P`, NAE triggers,
* otherwise use base scheduler.

---

### 9.5 NAE Selection Rule

If NAE triggers:

* select entry with **highest priority**
* tie-break deterministically (e.g., oldest insertion).

Upon selection:

* priority is halved,
* if priority < 2% → remove entry.

Priority decay occurs **even if** final emission is replaced by fallback.

---

### 9.6 NAE + Repeat Avoidance

If NAE-selected artwork matches `last_played_id`:

* attempt **one fallback** via base scheduler,
* if fallback still repeats → allow repeat.

---

### 9.7 PRNG Separation

* `PRNG_nae` is separate from `PRNG_randompick`.
* Seeds derived deterministically from `(global_seed, epoch_id, constant)`.

---

## 10. Streaming Generator Behavior

### 10.1 Generation Rule

Generation only occurs:

* inside `next()`,
* when lookahead has fewer than L items,
* in batches of exactly L items.

### 10.2 Generation Order

For each generated item:

1. NAE check (if enabled)
2. Base channel scheduling
3. Per-channel picking
4. Immediate repeat avoidance
5. Commit candidate to lookahead

---

## 11. Reference State Machine

### States

* READY
* RESET

(Generation is embedded inside `next()`; no separate shadow states exist.)

### Events

* `next()`
* `prev()`
* `peek(n)`
* `NAE_insert`
* `reset`

### Transitions

**READY**

* `reset` → RESET
* `prev()` → walk history backward
* `peek(n)` → read-only lookahead access
* `next()`:

  * if serving history → walk forward
  * else:

    * if lookahead.size < L → generate L items
    * pop one from lookahead and commit to history

**RESET**

* clear history, lookahead, credits, cursors, NAE pool
* `epoch_id++`
* → READY

---

## 12. Performance Constraints

* Prefer SD block reads (4–8 KB).
* Cache SD blocks in PSRAM per channel.
* Avoid any SD I/O during `prev()` or `peek()`.

---

## 13. Edge Cases

* **N = 1**: scheduler degenerates gracefully.
* **Tiny caches**: repeats expected.
* **Offline channels**: excluded.
* **NAE bursts**: bounded by J and decay.

