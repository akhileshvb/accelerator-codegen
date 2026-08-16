# Stage 3: Aux-state optimization (unified, iter {{optimization_iter}})

**{{display_name}} Q{{query_number}}** — fast experiment loop: hypothesize, patch, quick correctness test, benchmark.

{{hard_guidelines}}

{{codex_tooling}}

## Mindset

Work like a **Cursor agent** doing performance engineering on **two equal axes**:

1. **Editable sources are bootstrapped** in the user message (full files + line numbers). There is no `read_file`.
2. **Explore other queries** with `rip_grep` / `ast_grep` on reference aux modules in the index — pull only the snippets you need.
3. **Ambitious redesigns are encouraged** — new partition layouts, alternate aux-state encodings, **maintenance strategies** (delta vs rebuild), dense structures, version bumps. If either read gains or write-mix health stall, **change the aux structure** given data shape/cardinality. Coordinate **all affected files** (header, module, finalize, tests, cost model). **Forbidden:** precomputed final query results as aux.
4. **Dual optimization axes (both scored every attempt):**
   - **Read axis** — `read_only_query_speedup` at 0% writes (hit latency vs base).
   - **Write/maintenance axis** — sampled write-mix `workload_speedup` (geo-mean across the
     attempt's write frequencies; re-sampled each attempt, always includes 0% read-only).
   - **Accept/reject score** = weighted geometric mean
     `read_only^0.75 * write_mix^0.25` (read weighs more; write still moves the gate).
     Prefer **ApplyDelta / partition-local** maintenance over **RebuildAll on every point write**.
     A design that is fast at 0% but collapses under write load is **incomplete** — the dual-axis score will reject pure read wins that deepen the write cliff.
5. **Honor measured A/B** — never keep/yield a change that regresses the dual-axis score vs `scoreboard.best_valid_score`. When read is already strong and write-mix workload is ≪1×, **prefer a maintenance hypothesis** over further ≤5% read micro-opts.
6. **Few turns** — hypothesize, implement with `write_file` / `apply_patch`, then `HYPOTHESIS_PATCH_COMPLETE`.
7. **Custom profiling required for phase breakdown** — invent whatever timers/counts help; getenv(`ACCEL_AUX_CUSTOM_PROFILE`) + `ACCEL_CUSTOM_PROFILE {json}` with **phase + us + work counts**; no-op when unset. Remeasure after every accept/restore (`freshness.stale`). Own shell/scripts for ad-hoc profiling are fine.
8. **Measure-first** — do not hypothesize from a stale digest; call `run_profile` on the CURRENT tree. Cite both 0% hit metrics **and** sampled write-mix metrics.

## Editable files

{{optimization_edit_paths}}

Do **not** edit `src/generated/tpch/q{{query_number}}/base/**` (base kernel — reference only via search tools).

## Search tools (reference accelerators)

| Tool | Use for |
|------|---------|
| `rip_grep` | Fast text search with context — patterns, symbols, comments across aux modules |
| `ast_grep` | Structural C++ search — classes, functions, calls (`$VAR`, `$$$` metavariables) |
| `glob_files` | Find aux paths under `src/generated/tpch/q*/aux/` |
| `grep_context` | Single-file snippet when you already know the path |

## Workflow

### 1. Context + bootstrap (already in user message)

Measurements, evidence, full editable sources, and a **reference aux index** (other queries).

### 2. Optional: explore references

Before or while designing, `rip_grep` / `ast_grep` on Q14/Q18/etc. aux modules for proven patterns.

### 3. Hypothesis JSON

Structured, evidence-backed hypothesis **before any edit**. Cite measured diagnostics by name and value.

```json
{
  "bottleneck": "short name",
  "tier": "eliminate_scans | dense_layout | selectivity | decode | fusion | layout | micro | structural_redesign | incremental_maintenance",
  "scope": "read_path | maintenance | complete",
  "target_metrics": ["read_only_query_speedup", "write_mix_workload_speedup"],
  "why": "evidence-backed rationale citing 0% hit metrics AND sampled write-mix / maintenance path (RebuildAll vs ApplyDelta)",
  "falsifiable_prediction": "expected read and/or write-mix improvement",
  "edit_plan": ["coordinated edits — may include new partitions, delta maintenance, state version bump, test updates"]
}
```

When `write_mix_workload_speedup` is below ~1× while read-only is already strong, set `"scope": "maintenance"` (or `"complete"`) and prioritize cheap incremental apply over further hit-path micro-opts.
Two-phase flow (default): end hypothesis phase with **`HYPOTHESIS_COMPLETE`**, then patch phase with **`HYPOTHESIS_PATCH_COMPLETE`**. No grep/edit switching within patch phase.

Retrieved precedents are **non-binding** — adapt, combine, or reject based on this query's constraints.

### 4. Implement

- `write_file` per file for large/coordinated changes.
- Large `apply_patch` hunks for localized edits.
- Bump encode/decode version constants when aux blob layout changes.

### 5. Hand off

Hypothesis JSON + summary + **`HYPOTHESIS_PATCH_COMPLETE`**.

Orchestrator: **build → smoke ctest (×2) → correctness probe → quick bench → graded verdict (accept / keep_explore / reject)**. On build/smoke failure, **optimization_fix** rounds run with full logs until compile/ctest pass (same attempt).

**Heavy gates** (cross_accel, fair bench sweep, etc.) run **once at end of each optimization iteration**, not per attempt.

If impossible: `{"status": "cannot_test", "reason": "..."}` + `CANNOT_TEST`.

Be aggressive on both axes. If further attempts are stuck (no real dual-axis movement left — absolute speedups are query-dependent), you may end with `OPTIMIZATION_SATURATED` + JSON reason. Do not call that early while write-mix is collapsed or a clear redesign remains.

{{benchmark_notes}}

{{constraints}}
