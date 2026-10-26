# Stage 3: Aux-state optimization worker, stateful (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}**: inspect, patch, build, test and measure in one session, then yield a candidate.

## Goal

**Push both axes at SF3000: fast reads and cheap maintenance, from copy-on-write state, correctly.** The accept score is `read_only^0.75 * write_mix^0.25`: read-only speedup at 0% writes, and workload speedup over sampled write mixes (re-sampled every attempt). A read win that deepens the write cliff loses.

**Read floor: 20x over the engine's own SQL for the same binding.** Below it, saturation is refused: profile, then attack the hot spot (structural redesign of the state, or SIMD/layout work) until reads clear 20x.

1. **Low-level execution** on the read, build and maintain paths, driven by the profile: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/docs/low_level_execution.md`.
2. **State from the COW primitive library.** Reuse a catalogued primitive when it fits. Otherwise design a bespoke, generic COW primitive for this query template's access pattern and publish it with `primitive_publish` (additions only, generic, tested). Never hand-roll a private COW container in the module; never deep-copy state per commit. Maintenance cost is proportional to touched data. Guide: `pipeline/prompts/aux_state/docs/cow_primitives.md`.
3. **Structure over tweaks.** **If the profile shows the state is the problem, redesign it here; it must pass the full suite and beat the best to stay.** (The read visits too much state, or the layout defeats SIMD.) Any rewrite of the state organization and the read/maintain paths within the query's own files is allowed, including a new COW primitive through `primitive_publish`; the frozen test suite stays the contract. Declare it with `strategy: "structural"` and a `design_revision` {`profile_evidence`, `old_organization`, `new_organization`, `read_work_ratio` (new estimate, base work / design work)}. A structural attempt runs the full correctness tier (both thread counts), `aux_unit`, the race gate and the multi-connection gate before the benchmark, and is kept only if reads improve and the write axis holds; otherwise it is rolled back like any rejected attempt. Kept redesigns are written to the design record the audit judges.
    When a ceiling shows (reads stall, or write-mix stays ≪ 1x), redesign the layout, encoding or maintenance strategy for the data's shape. Forbidden: storing final query results. Prefer `ApplyDelta` and partition-local updates over `RebuildAll`. When read is strong and write-mix is collapsed, attack maintenance first (`maintenance_framework.md`, "Refresh-shaped writes").

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, `accelerators/prims/` except through `primitive_publish`. **Judged by:** the A/B against `scoreboard.best_valid_score`, then the post-iteration gates.

The test suite (`tests/workload/`) is frozen and `quick_check` runs it: never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md`.

{{hard_guidelines}}

Pipeline files (`pipeline/`, `substrate/`, `backends/` gates and prompts, `cmake/`) are not yours: edits there are reverted and warned about in tool footers; if the task is blocked on one, call `report_blocker`.


{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

## Flow

1. `run_profile` on the CURRENT tree; read `scoreboard` (the read axis against the engine's own SQL), the hot functions, counters, visited-state count and `freshness`. Cite both 0% hit metrics and sampled write-mix metrics. Start every attempt from the `run_profile` output (hot functions, hardware counters, visited-state count). Name the hot spot you target (`hot_spot`), the gain you expect there (`expected_gain`) and the `strategy`: `structural` (state organization or algorithm), `low_level` (SIMD with a scalar fallback, branch-free predicates, compact columnar layout, prefetch, no allocation or indirection in the hot loop) or `other`. Then re-measure and repeat on the next hot spot.
2. State a hypothesis from named phases and counts of a fresh digest.
3. Implement the coordinated edit across all affected files (header, module, finalize, tests, cost model); bump encode/decode version constants when the blob layout changes. `begin_ab_trial(trial_id, hypothesis, hot_spot, expected_gain, strategy)` first (it refuses without a fresh `run_profile`); `quick_check` after each edit until it passes with no projected-write-cost failure; the heavy gates are refused before that.
4. `run_quick_benchmark` for direction, then `run_quick_benchmark(acceptance=True)`, then `end_ab_trial`. `keep=True` needs the acceptance run. If `ab_signal` is `regress`, `forced_discard` or `kept=false`, do not yield: form a new hypothesis.
5. Optimizations do not trade away generality: keep `hard_guidelines.md` §4, the scale failure classes (`scale_factor_contract.md`) and the thread rules (results identical at any cap). Re-run the parameter, magnitude-database and 1-vs-N thread differentials after structural changes.
6. End with one marker (a task that ends without one fails the attempt):
   - Hypothesis JSON `{"bottleneck": "...", "why": "...", "expected_effect": "...", "ab_signal": "improve|regress|skipped"}` and `CANDIDATE_READY`, only after an A/B that did not regress.
   - `CANNOT_TEST` with `{"status": "cannot_test", "reason": "..."}`.
   - `OPTIMIZATION_SATURATED` with `{"status": "optimization_saturated", "reason": "...", "remaining_headroom": "low"}`, once bold attempts on both axes no longer move the score. Not while write-mix is collapsed or a clear redesign is untried. While the read speedup over the engine's own SQL is below the floor in the scoreboard, the orchestrator rejects it unless the last trials were each profile-guided and distinct, with one `structural` and one `low_level`.

The orchestrator then runs the authoritative gates. Before relying on a bench number, check it measured the accelerator (below).

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
