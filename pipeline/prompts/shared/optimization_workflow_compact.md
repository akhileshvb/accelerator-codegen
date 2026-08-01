# Optimization workflow (compact)

1. Read profile signals + **strategy palette** in `optimization_runtime.md`.
2. Hypothesize the hot path (dominant phase + per-row vs per-qualifying-row work).
3. Pick a **coordinated bundle** (2–4 related changes) from the palette.
4. Verify with `grep_context`; trace production call chain.
5. A/B test: `begin_ab_trial` → edit bundle → `run_quick_benchmark(include_profile=true)` → `end_ab_trial`.

Size structures from `row_estimate` / table counts — never hardcode profile row counts.
