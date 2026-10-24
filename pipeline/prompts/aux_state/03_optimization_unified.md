# Stage 3: Aux-state optimization, stateful (unified, iter {{optimization_iter}})

**{{display_name}} Q{{query_number}}**: hypothesize, patch, quick correctness test, benchmark.

## Goal

**Push both axes at SF3000: fast reads and cheap maintenance, from copy-on-write state, correctly.** The accept score is `read_only^0.75 * write_mix^0.25`: read-only speedup at 0% writes, and workload speedup over sampled write mixes (re-sampled every attempt). A read win that deepens the write cliff loses.

**Read floor: 20x over the stateless kernel.** Below it, saturation is refused: profile, then attack the hot spot (structural redesign of the state, or SIMD/layout work) until reads clear 20x.

1. **Low-level execution** on the read, build and maintain paths, driven by the profile: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`.
2. **State from the COW primitive library.** Reuse a catalogued primitive when it fits. Otherwise design a bespoke, generic COW primitive for this query template's access pattern and publish it with `primitive_publish` (additions only, generic, tested). Never hand-roll a private COW container in the module; never deep-copy state per commit. Maintenance cost is proportional to touched data. Guide: `pipeline/prompts/aux_state/shared/cow_primitives.md`.
3. **Structure over tweaks.** **If the profile shows the state is the problem, redesign it here; it must pass the full suite and beat the best to stay.** (The read visits too much state, or the layout defeats SIMD.) Any rewrite of the state organization and the read/maintain paths within the query's own files is allowed, including a new COW primitive through `primitive_publish`; the frozen test suite stays the contract. Declare it with `strategy: "structural"` and a `design_revision` {`profile_evidence`, `old_organization`, `new_organization`, `read_work_ratio` (new estimate, base work / design work)}. A structural attempt runs the full correctness tier (both thread counts), `aux_unit`, the race gate and the multi-connection gate before the benchmark, and is kept only if reads improve and the write axis holds; otherwise it is rolled back like any rejected attempt. Kept redesigns are written to the design record the audit judges.
    When a ceiling shows (reads stall, or write-mix stays ≪ 1x), redesign the layout, encoding or maintenance strategy for the data's shape. Forbidden: storing final query results. Prefer `ApplyDelta` and partition-local updates over `RebuildAll`. When read is strong and write-mix is collapsed, attack maintenance first (`maintenance_framework.md`, "Refresh-shaped writes").

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, the query's `base/**` kernel, `accelerators/prims/` except through `primitive_publish`. **Judged by:** build, smoke ctest x2, a correctness probe and the quick bench with a graded verdict (accept / keep_explore / reject) after your handoff; `optimization_fix` rounds repair build or smoke failures; the heavy gates run once at the end of the iteration.

The test suite (`tests/workload/`) is frozen and `quick_check` runs it: never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md`.

{{hard_guidelines}}

Pipeline files (`pipeline/`, `substrate/`, `backends/` gates and prompts, `cmake/`) are not yours: edits there are reverted and warned about in tool footers; if the task is blocked on one, call `report_blocker`.

{{codex_tooling}}

{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

The sources are bootstrapped in the user message with line numbers; do not re-read them. Search reference modules with `rg` (`accelerators/tpch/q*/<engine>/aux/`) and `sed -n`. Retrieved precedents are non-binding.

## Flow

Optimizations do not trade away generality: keep `hard_guidelines.md` §4, the scale failure classes (`scale_factor_contract.md`) and the thread rules (results identical at any cap); re-run the parameter, magnitude-database and 1-vs-N thread differentials after structural changes.

1. **Hypothesis phase.** `run_profile` on the CURRENT tree (hot functions, counters, visited-state count), then write the hypothesis JSON before any edit, citing measured diagnostics by name and value, 0% hit metrics and sampled write-mix metrics both. End with **`HYPOTHESIS_COMPLETE`**.

```json
{
  "bottleneck": "short name",
  "hot_spot": "the hot function or loop from the run_profile output that this targets",
  "expected_gain": "the gain you expect there, in us or %",
  "strategy": "structural | low_level | other",
  "tier": "eliminate_scans | dense_layout | selectivity | decode | fusion | layout | micro | structural_redesign | incremental_maintenance",
  "scope": "read_path | maintenance | complete",
  "target_metrics": ["read_only_query_speedup", "write_mix_workload_speedup"],
  "why": "evidence-backed rationale citing 0% hit metrics AND the write-mix / maintenance path",
  "falsifiable_prediction": "expected read and/or write-mix improvement",
  "edit_plan": ["coordinated edits: partitions, delta maintenance, version bump, tests"],
  "design_revision": {"profile_evidence": "...", "old_organization": "...", "new_organization": "...", "read_work_ratio": "new estimate: base work / design work"}   // strategy=structural only
}
```

2. **Patch phase.** Rewrite whole files for coordinated changes, patch for local ones; bump encode/decode version constants when the blob layout changes. No grep/edit switching: implement, `quick_check`, then end with the hypothesis JSON, a short summary and **`HYPOTHESIS_PATCH_COMPLETE`**.
3. If impossible: `{"status": "cannot_test", "reason": "..."}` and `CANNOT_TEST`. If both axes are exhausted (not while write-mix is collapsed or a clear redesign remains): `OPTIMIZATION_SATURATED` with a JSON reason. Below the read-speedup floor it is honored only after profile-guided, distinct trials including one `structural` and one `low_level`.

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
