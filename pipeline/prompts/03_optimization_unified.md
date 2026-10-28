# Stage 3: Optimization, stateless (unified, iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}**: one hypothesis, one coordinated patch, validated by the orchestrator.

## Goal

**Push the kernel to machine speed: maximize `speedup_median` over the engine's own path, correctly, at SF3000.** There is no fixed ceiling.

- Fix the hot phase the profile names with hardware-aware low-level execution: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious columnar layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`; loop and anti-patterns: `pipeline/prompts/shared/base_kernel_optimization.md`.
- Prefer a coordinated structural bundle over micro-tweaks, and keep it correct and winning at SF3000.

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, `accelerators/prims/`. **Judged by:** build, quick correctness and the benchmark after your handoff; `optimization_fix` rounds repair build or test failures.

The test suite (`tests/workload/`) is frozen and `quick_check` runs it: never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md`.

{{hard_guidelines}}

Pipeline files (`pipeline/`, `substrate/`, `backends/` gates and prompts, `cmake/`) are not yours: edits there are reverted and warned about in tool footers; if the task is blocked on one, call `report_blocker`.


{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

The sources are bootstrapped in the user message; use the MCP digests instead of re-reading everything, and search reference accelerators with `rg` and `sed -n`.

## Flow

1. `run_profile`, cite the profile evidence (named phases and counts), then write the hypothesis JSON before any edit; it must include `hot_spot`, `expected_gain` and `strategy` (`structural` | `low_level` | `other`). Start every attempt from the `run_profile` output (hot functions, hardware counters, visited-state count). Name the hot spot you target (`hot_spot`), the gain you expect there (`expected_gain`) and the `strategy`: `structural` (state organization or algorithm), `low_level` (SIMD with a scalar fallback, branch-free predicates, compact columnar layout, prefetch, no allocation or indirection in the hot loop) or `other`. Then re-measure and repeat on the next hot spot.
2. Make the coordinated edits; `quick_check` after each.
3. End with the hypothesis JSON, a short summary and `HYPOTHESIS_PATCH_COMPLETE`.

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
