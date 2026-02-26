# Deep Codebase Audit — February 2026

Four parallel analysis agents tore through the entire p3a codebase simultaneously,
hunting for concurrency bugs, memory leaks, security vulnerabilities, and
architectural issues.

**~30 findings across 4 categories.**

## Reports

| Report | Findings | Highlights |
|--------|----------|------------|
| [Concurrency](concurrency.md) | 10 | Prefetch use-after-free, ISR race, MQTT deadlock |
| [Memory & Resources](memory.md) | 8 | Upload buffer leak, PNG decoder leak, stack overflow risk |
| [Security](security.md) | 15 | No HTTP auth, path traversal, filename injection |
| [Architecture](architecture.md) | 7 | Silent boot failures, dead state, empty playlist deadlock |

## Top 5 Most Actionable Fixes

1. **Path traversal + filename injection** in HTTP API — straightforward to fix, high impact
2. **PNG decoder `free(dec)` missing** — one-line fix for a real memory leak
3. **`chunk_buffer` leak on HTTP client init failure** — add `free()` before early returns
4. **Triple-buffer ISR race** — switch to C11 atomics for `g_displaying_idx` and buffer state
5. **MQTT callback deadlock** — queue events to a separate task instead of dispatching from handler
