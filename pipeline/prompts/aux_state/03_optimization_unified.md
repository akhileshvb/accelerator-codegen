# Stage 3: Aux-state optimization (unified, iter {{optimization_iter}})

**{{display_name}} Q{{query_number}}** — fast experiment loop: hypothesize, patch, quick correctness test, benchmark.

## Mindset

Work like a **Cursor agent** doing performance engineering:

1. **Editable sources are bootstrapped** in the user message (full files + line numbers). There is no `read_file`.
2. **Explore other queries** with `rip_grep` / `ast_grep` on reference aux modules in the index — pull only the snippets you need.
3. **Ambitious redesigns are encouraged** — new partition layouts, alternate aux-state encodings, maintenance strategies, dense structures, version bumps. Coordinate **all affected files** (header, module, finalize, tests, cost model).
4. **Read path is the score** — `read_only_query_speedup` at 0% writes. Maintenance must stay correct (orchestrator runs quick ctest after each patch).
5. **Few turns** — hypothesize, implement with `write_file` / `apply_patch`, then `HYPOTHESIS_PATCH_COMPLETE`.

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
  "tier": "eliminate_scans | dense_layout | selectivity | decode | fusion | layout | micro | structural_redesign",
  "scope": "read_path | maintenance | complete",
  "target_metrics": ["read_only_query_speedup", "validity_slots_scanned_median"],
  "why": "evidence-backed rationale citing store_diagnostics",
  "falsifiable_prediction": "expected read-path or validity-scan improvement",
  "edit_plan": ["coordinated edits — may include new partitions, state version bump, test updates"]
}
```

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

{{benchmark_notes}}

{{constraints}}
