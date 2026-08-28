# RUN-20260828-00-validate — instrumentation validation (not a soak)

- Build: diag (`build-diag`, feat/jitter @ Phase 1), playset "Work mix", 129 s of frames.
- Purpose: prove detector → UART report → HTTP pull → analyzer chain; probe provoke kinds.
- Result: chain works. `cpu1` 300 ms hog → 177 ms stall, attributed. `nvs`×3 and
  `sd`×8 produced nothing; `log`×30 pushed producer time up (5 extra overrun frames,
  worst-10 s 54 ms).
- Baseline lateness (valid frames 1438): p50 0.2 ms, p90 7.0 ms, p99 8.5 ms, max 177 ms (the provoked one).
- Producer time: p50 21 ms, p90 47 ms, p99 85 ms, max 316 ms (one artwork is a genuine
  overrun case; out of scope).
- Defects found: `JTR|T core=-1` (fixed via `FREERTOS_VTASKLIST_INCLUDE_COREID`);
  analyzer initially classed starved producers as overrun (fixed with the
  per-generation median rule).
