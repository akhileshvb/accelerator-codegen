# Stage 3: Aux-state optimization, stateful (unified, iter {{optimization_iter}})

**{{display_name}} Q{{query_number}}**: hypothesize, patch, quick correctness test, benchmark.

## Goal

**Push both axes at SF3000: fast reads and cheap maintenance, from copy-on-write state, correctly.** The accept score is `read_only^0.75 * write_mix^0.25`: read-only speedup at 0% writes, and workload speedup over sampled write mixes (re-sampled every attempt). A read win that deepens the write cliff loses.

1. **Low-level execution** on the read, build and maintain paths, driven by the profile: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`.
2. **State from the COW primitive library.** Reuse a catalogued primitive when it fits. Otherwise design a bespoke, generic COW primitive for this query template's access pattern and publish it with `primitive_publish` (additions only, generic, tested). Never hand-roll a private COW container in the module; never deep-copy state per commit. Maintenance cost is proportional to touched data. Guide: `pipeline/prompts/aux_state/shared/cow_primitives.md`.
3. **Structure over tweaks.** When a ceiling shows (reads stall, or write-mix stays ≪ 1x), redesign the layout, encoding or maintenance strategy for the data's shape. Forbidden: storing final query results. Prefer `ApplyDelta` and partition-local updates over `RebuildAll`. When read is strong and write-mix is collapsed, attack maintenance first (`maintenance_framework.md`, "Refresh-shaped writes").

**Owns:** the editable files below. **Never touches:** `backends/`, `substrate/`, other queries, the query's `base/**` kernel, `accelerators/prims/` except through `primitive_publish`. **Judged by:** build, smoke ctest x2, a correctness probe and the quick bench with a graded verdict (accept / keep_explore / reject) after your handoff; `optimization_fix` rounds repair build or smoke failures; the heavy gates run once at the end of the iteration.

{{hard_guidelines}}

{{codex_tooling}}

{{fast_loop}}

## Editable files

{{optimization_edit_paths}}

The sources are bootstrapped in the user message with line numbers; do not re-read them. Search reference modules with `rg` (`accelerators/tpch/q*/<engine>/aux/`) and `sed -n`. Retrieved precedents are non-binding.

## Flow

Optimizations do not trade away generality: keep `hard_guidelines.md` §4, the scale failure classes (`scale_factor_contract.md`) and the thread rules (results identical at any cap); re-run the parameter, magnitude-database and 1-vs-N thread differentials after structural changes.

1. **Hypothesis phase.** `run_profile` on the CURRENT tree, then write the hypothesis JSON before any edit, citing measured diagnostics by name and value, 0% hit metrics and sampled write-mix metrics both. End with **`HYPOTHESIS_COMPLETE`**.

```json
{
  "bottleneck": "short name",
  "tier": "eliminate_scans | dense_layout | selectivity | decode | fusion | layout | micro | structural_redesign | incremental_maintenance",
  "scope": "read_path | maintenance | complete",
  "target_metrics": ["read_only_query_speedup", "write_mix_workload_speedup"],
  "why": "evidence-backed rationale citing 0% hit metrics AND the write-mix / maintenance path",
  "falsifiable_prediction": "expected read and/or write-mix improvement",
  "edit_plan": ["coordinated edits: partitions, delta maintenance, version bump, tests"]
}
```

2. **Patch phase.** Rewrite whole files for coordinated changes, patch for local ones; bump encode/decode version constants when the blob layout changes. No grep/edit switching: implement, `quick_check`, then end with the hypothesis JSON, a short summary and **`HYPOTHESIS_PATCH_COMPLETE`**.
3. If impossible: `{"status": "cannot_test", "reason": "..."}` and `CANNOT_TEST`. If both axes are exhausted (not while write-mix is collapsed or a clear redesign remains): `OPTIMIZATION_SATURATED` with a JSON reason.

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
