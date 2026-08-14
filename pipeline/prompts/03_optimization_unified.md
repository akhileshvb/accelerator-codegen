# Stage 3: Optimization (unified, iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — fast experiment loop with bootstrapped sources and search tools.

## Mindset

1. **Editable sources are in the user message** — no `read_file`.
2. **`rip_grep` / `ast_grep`** — explore reference kernels and other aux modules.
3. **Ambitious structural changes allowed** when evidence supports them.
4. Orchestrator runs **build → quick correctness (ctest) → benchmark** after handoff. On failure, **optimization_fix** rounds debug with full logs until build/ctest pass.

## Editable files

{{optimization_edit_paths}}

## Workflow

1. Use bootstrap + context packet in the user message.
2. Optionally search references with `rip_grep` / `ast_grep`.
3. Hypothesis JSON + coordinated edits (`write_file` / `apply_patch`).
4. `HYPOTHESIS_PATCH_COMPLETE`.

{{benchmark_notes}}

{{constraints}}
