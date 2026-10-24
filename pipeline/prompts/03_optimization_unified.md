# Stage 3: Optimization (unified, iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — fast experiment loop with bootstrapped sources and search tools.

{{hard_guidelines}}

{{codex_tooling}}

## Mindset

1. **Editable sources are in the user message** when bootstrapped — prefer MCP digests over re-reading everything.
2. Search reference accelerators with the shell (`rg`, `sed -n`).
3. **Ambitious structural changes allowed** when evidence supports them.
4. Orchestrator runs **build → quick correctness → benchmark** after handoff. On failure, **optimization_fix** rounds debug until build/ctest pass.

## Editable files

{{optimization_edit_paths}}

## Workflow

1. Use bootstrap + context packet + profile/evidence digests.
2. Hypothesis JSON + coordinated edits.
3. `HYPOTHESIS_PATCH_COMPLETE`.

{{measurement_trust}}

{{benchmark_notes}}

{{constraints}}
