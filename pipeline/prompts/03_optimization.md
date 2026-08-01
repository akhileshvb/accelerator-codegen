# Stage 3: Optimization (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — {{optimization_goal}}

## State (read every episode — not chat history)

**`{{optimization_runtime_path}}`** — profile counters, phase timings, **strategy palette**, suggested bundle, A/B trials, episode history.

Do **not** read `optimization_feedback.md`, `baseline_summary.json`, `profile_summary.json`, or `base_kernel_optimization.md` unless runtime points you there.

{{kernel_inspiration_section}}

## Workflow

1. **Read profile** — counters, phase us, dominant phase, and ratios in runtime state.
2. **Pick a coordinated bundle** — 2–4 related changes from the strategy palette (★ = boosted by profile signals; other options are equally valid if they match your hypothesis).
3. **Verify** — `grep_context` on `{{kernel_path}}`; trace fused/scan → consume → aggregate.
4. **A/B test** — `begin_ab_trial` → edit bundle → `run_quick_benchmark(include_profile=true)` → `end_ab_trial`.

Ship aggressive bundles when speedup is far below 1.0x — micro-opts won't close a 10× gap.

Forbidden: aux-state, persistent indexes, cross-query cache, production `getenv`.

{{benchmark_notes}}

{{constraints}}
