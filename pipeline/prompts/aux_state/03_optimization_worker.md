# Stage 3: Aux-state optimization worker, stateful (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}**: inspect, patch, build, test and measure in one session, then yield a candidate.

## Goal

**Push both axes at SF3000: fast reads and cheap maintenance, from copy-on-write state, correctly.** The accept score is `read_only^0.75 * write_mix^0.25`: read-only speedup at 0% writes, and workload speedup over sampled write mixes (re-sampled every attempt). A read win that deepens the write cliff loses.

1. **Low-level execution** on the read, build and maintain paths, driven by the profile: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`.
2. **State from the COW primitive library.** Reuse a catalogued primitive when it fits. Otherwise design a bespoke, generic COW primitive for this query template's access pattern and publish it with `primitive_publish` (additions only, generic, tested). Never hand-roll a private COW container in the module; never deep-copy state per commit. Maintenance cost is proportional to touched data. Guide: `pipeline/prompts/aux_state/shared/cow_primitives.md`.
3. **Structure over tweaks.** When a ceiling shows (reads stall, or write-mix stays ≪ 1x), redesign the layout, encoding or maintenance strategy for the data's shape. Forbidden: storing final query results. Prefer `ApplyDelta` and partition-local updates over `RebuildAll`. When read is strong and write-mix is collapsed, attack maintenance first (`maintenance_framework.md`, "Refresh-shaped writes").

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, the query's `base/**` kernel, `accelerators/prims/` except through `primitive_publish`. **Judged by:** the A/B against `scoreboard.best_valid_score`, then the post-iteration gates.

{{hard_guidelines}}

{{codex_tooling}}

{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

## Flow

1. `run_profile` on the CURRENT tree; read `scoreboard`, `phase_breakdown` and `freshness`. Cite both 0% hit metrics and sampled write-mix metrics.
2. State a hypothesis from named phases and counts of a fresh digest.
3. Implement the coordinated edit across all affected files (header, module, finalize, tests, cost model); bump encode/decode version constants when the blob layout changes. `begin_ab_trial` first; `quick_check` after each edit.
4. `run_quick_benchmark` for direction, then `run_quick_benchmark(acceptance=True)`, then `end_ab_trial`. `keep=True` needs the acceptance run. If `ab_signal` is `regress`, `forced_discard` or `kept=false`, do not yield: form a new hypothesis.
5. Optimizations do not trade away generality: keep `hard_guidelines.md` §4, the scale failure classes (`scale_factor_contract.md`) and the thread rules (results identical at any cap). Re-run the parameter, magnitude-database and 1-vs-N thread differentials after structural changes.
6. End with one marker (a task that ends without one fails the attempt):
   - Hypothesis JSON `{"bottleneck": "...", "why": "...", "expected_effect": "...", "ab_signal": "improve|regress|skipped"}` and `CANDIDATE_READY`, only after an A/B that did not regress.
   - `CANNOT_TEST` with `{"status": "cannot_test", "reason": "..."}`.
   - `OPTIMIZATION_SATURATED` with `{"status": "optimization_saturated", "reason": "...", "remaining_headroom": "low"}`, once bold attempts on both axes no longer move the score. Not while write-mix is collapsed or a clear redesign is untried.

The orchestrator then runs the authoritative gates. Before relying on a bench number, check it measured the accelerator (below).

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
