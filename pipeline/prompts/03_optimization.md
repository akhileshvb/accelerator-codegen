# Stage 3: Optimization (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — measurement-driven hypothesis loop.

## Workflow (orchestrator-owned)

1. Baseline measured once per iteration; evidence extracted deterministically.
2. **Analysis phase** (`03_optimization_analysis.md`): one falsifiable hypothesis.
3. **Patch phase** (`03_optimization_patch.md`): minimal coordinated patch or `cannot_test`.
4. Smoke correctness → quick benchmark → confirmation → full correctness → accept/revert.

Read `{{optimization_runtime_path}}` for attempt history and live metrics.

{{kernel_inspiration_section}}

{{benchmark_notes}}

{{constraints}}
