# Stage 3: Optimization worker, stateless (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}**: inspect, patch, build, test and measure in one session, then yield a candidate.

## Goal

**Push the kernel to machine speed: maximize `speedup_median` over the engine's own path, correctly, at SF3000.** There is no fixed ceiling.

- The profile names the hot phase. Fix it with hardware-aware low-level execution: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious columnar layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`; loop and anti-patterns: `pipeline/prompts/shared/base_kernel_optimization.md`.
- Prefer a coordinated structural bundle (scan order, interning, fusion, vector paths) over micro-tweaks.
- A bundle must stay correct and still win at SF3000 (`scale_factor_contract.md`).

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, `accelerators/prims/`. **Judged by:** the A/B against `scoreboard.best_valid_score`, then the post-iteration gates.

The test suite (`tests/workload/`) is frozen and `quick_check` runs it: never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md`.

{{hard_guidelines}}

{{codex_tooling}}

{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

## Flow

1. `run_profile` on the CURRENT tree; read `scoreboard`, `phase_breakdown` and `freshness`. A digest that is empty or stale is not evidence.
2. State a hypothesis citing named phases and counts from the fresh digest.
3. Implement the coordinated edit. `begin_ab_trial` first; `quick_check` after each edit.
4. `run_quick_benchmark` for direction, then `run_quick_benchmark(acceptance=True)`, then `end_ab_trial`. `keep=True` needs the acceptance run. If `ab_signal` is `regress`, `forced_discard` or `kept=false`, do not yield: form a new hypothesis.
5. End with one marker (a task that ends without one fails the attempt):
   - Hypothesis JSON `{"bottleneck": "...", "why": "...", "expected_effect": "...", "ab_signal": "improve|regress|skipped"}` and `CANDIDATE_READY`, only after an A/B that did not regress.
   - `CANNOT_TEST` with `{"status": "cannot_test", "reason": "..."}`.
   - `OPTIMIZATION_SATURATED` with `{"status": "optimization_saturated", "reason": "...", "remaining_headroom": "low"}`, once structural bundles no longer move `speedup_median`. Not while a clear redesign is untried.

The orchestrator then runs the authoritative gates. Before relying on a bench number, check it measured the accelerator (below).

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
