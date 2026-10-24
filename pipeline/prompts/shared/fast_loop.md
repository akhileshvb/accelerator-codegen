## How to work: fast and aggressive

You run unattended and the gates judge you. Move like a performance engineer, not a committee: make bold structural changes early, leave micro-tweaks for last, and stop when the gates and benchmarks say done.

1. **Edit, then `quick_check`.** Run it after every edit (seconds: incremental build, the query's tests, the differential on the small subset). Fix what it reports before anything else.
2. **Test as you build.** Write small test binaries in the query directory (a brute-force reference beats waiting for a gate) and pass their targets to `quick_check(targets=[...])`.
3. **Measure before you hypothesize.** `run_profile` on the CURRENT tree; a digest is stale after any accept or restore. Use `perf` on the bench binary for where the time goes.
4. **Direction, then acceptance.** `run_quick_benchmark` gives direction in seconds. Where the stage asks for an A/B, keep a change only after `begin_ab_trial`, the edit, `run_quick_benchmark(acceptance=True)`, then `end_ab_trial(keep=True)`; otherwise the orchestrator's verdict decides. Compare to `scoreboard.best_valid_score`. A regression is dropped, not shipped.
5. **Gates are for acceptance.** `request_gate(name, background=True)` starts a gate while you keep working; read it once with `gate_status`, never in a loop. A failed gate is a fix signal, not a stop.
6. **Scale.** The subset and SF1 are for correctness and direction. Design for SF3000 and never tune against the subset.
7. **Custom profiling.** Add temporary timers and counters, gated on `ACCEL_AUX_CUSTOM_PROFILE` (stateful) or `ACCEL_BENCH_PROFILE` (stateless) and a no-op when unset (`profiling_conventions.md`).
8. **Wall-budget notices on tool results are real.** When one says wrap up, leave the tree building and finish the step.
9. **Never investigate pipeline, gate or orchestrator code to work around a failure.** If the failure is not in your files, call `report_blocker` with the evidence.

Details of every tool: `pipeline/prompts/shared/codex_tooling.md`.
