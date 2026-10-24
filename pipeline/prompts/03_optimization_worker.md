# Stage 3: Optimization worker (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — inspect, patch, build, test, and measure in one session before yielding a candidate.

{{hard_guidelines}}

{{codex_tooling}}

## Mindset

1. **Measure first on the CURRENT tree** — call MCP `run_profile` (or `get_profile_digest` then `run_profile` if `freshness.stale`) before forming a hypothesis. Do not reuse phase breakdowns from a prior accept/restore.
2. **Inspect** editable files and digests (`get_profile_digest` / `get_optimization_evidence` / `get_store_diagnostics`). Digests must show real metrics plus a **scoreboard** (`best_valid_score`). If empty or stale, remeasure.
3. **Be aggressive + scale-safe on both axes** — push **hard** for large read-only speedups **and** cheap incremental maintenance that still look strong at **high SF** (up to SF3000). Prefer structural layout/encoding/partition/decode **or** delta-maintenance redesigns; tiny micro-shuffles are usually the wrong tier. Absolute numbers are query-dependent (some queries top out lower but still win big at high scale).
4. **Ceiling → redesign aux structure** — if read gains stall **or** write-mix stays collapsed, change the aux-state layout / maintenance strategy given data shape/cardinality. **Forbidden:** storing precomputed **final query results** as aux. Intermediates (indexes, aggregates, eligibility, membership) are OK.
5. **Write path is a first-class goal** — optimize bulk refresh/`Rebuild*` **and** point/synthetic **ApplyDelta** paths; prefer local incremental updates over full rebuilds when deltas allow. Quick bench includes **sampled write mixes** (not a fixed 10%). If `write_mix_workload_speedup` ≪ 1× while hit latency is already tiny, **do not** spend the attempt on further read micro-opts — fix maintenance. Judge maintenance on refresh-shaped writes (bulk ~0.1% inserts/deletes, keys spread uniformly, so nearly every unit is touched): COW unit size, per-touch index rebuilds, size-guard `RebuildAll`s, base-table rescans and cost-model calibration (`maintenance_framework.md`, "Refresh-shaped writes").
5b. **Copy-on-write from `prims/`** — new state layouts are built from the shared primitives (consult the `prims/README.md` catalogue, which may be empty, before writing a helper). If you need a generic engine-neutral container none covers, stage it (header + randomized tests as full files under `pipeline/prims_staging/<repo-relative path>`) and publish it with `primitive_publish` before using it (it adds the catalogue row and rejects duplicates: reuse or extend; additions only, generic only; `prims/` is not directly writable). Never hand-roll a private COW container or deep-copy the state per commit.
6. **Iterate locally** — `quick_check` after every edit (aux-state: build, the query's tests and the subset differential in seconds), allowlisted build/ctest (`run_pipeline_command` or Codex shell). Write small test binaries in your query directory and use `perf` on the bench binary directly. You may write your own shell commands and short scripts to inspect, profile, or debug; fix failures before signaling ready. The subset and SF1 are for correctness and direction: design for SF3000, never tune against them.
7. **Exploratory A/B is binding for yield decisions** — MCP `begin_ab_trial` → edit → `quick_check` → `run_quick_benchmark` (direction) → `run_quick_benchmark(acceptance=True)` → `end_ab_trial`; `end_ab_trial(keep=True)` needs the acceptance run:
   - Compare against **`scoreboard.best_valid_score` only** (not ephemeral local baselines).
   - If `ab_signal=regress` / `forced_discard` / `kept=false`: **do not** yield that change; form a new hypothesis.
   - Only yield `CANDIDATE_READY` after an exploratory keep/improve (or after A/B was impossible and you explain why).
8. Orchestrator runs **authoritative** gates after `CANDIDATE_READY`. Before relying on a bench number, check it measured the accelerator (`pipeline/prompts/shared/measurement_trust.md`).
9. **Early stop when stuck** — be aggressive first. If you have already pushed both axes hard and further attempts are not moving the dual-axis score (or only trivial micro-noise), end with `OPTIMIZATION_SATURATED`. There is **no target speedup number** — call it when juice is gone for this query, not because a threshold was hit.

**Hard stop rule:** never end the Codex task without `CANDIDATE_READY`, `CANNOT_TEST`, or `OPTIMIZATION_SATURATED`. Cleanup edits after tests still require a marker — missing it fails the attempt.

## Custom profiling (required for fresh phase breakdown)

You are **free to invent whatever profiling you need**: add/change/remove timers, counters, phases, and helper scripts. Prefer temporary instrumentation in editable sources, gated on `getenv("ACCEL_AUX_CUSTOM_PROFILE")` (aux) or `getenv("ACCEL_BENCH_PROFILE")` (base) — **no-op when unset**. Emit split timers **and** work counts:

```
ACCEL_CUSTOM_PROFILE {"phase":"sort","us":120.5,"emitted_rows":18000,"groups":18000}
ACCEL_CUSTOM_PROFILE {"phase":"aggregate","us":40.0,"rows":50000,"groups":1200}
```

Rules:
- One JSON object per timed region; include `phase` + `us` (or `*_us`) plus counts (`rows`/`groups`/`items`/`bytes`/…).
- Digests aggregate into `phase_breakdown`, `work_counters_mean`, `dominant_phase`, and `residual_vs_hit_us`.
- Hypothesis must cite **named phases and counts** from a **non-stale** digest — not guesses.
- After every accept/restore, profile is stale until `run_profile` runs again.
- Shell/scripts for ad-hoc measurement are encouraged when MCP digests are too coarse.

## Editable files

{{optimization_edit_paths}}

## Workflow

1. Review seed context and prior gate feedback in the user message.
2. `run_profile` on CURRENT tree → read `scoreboard` + `phase_breakdown` (+ freshness).
3. Hypothesize from **measured** evidence (cite metric names and values).
4. Implement coordinated edits aimed at a large, **high-SF-safe** win (read path and/or maintenance). Redesign aux structure if needed — never cache final answers.
5. Build, smoke test, **required** exploratory A/B when MCP is up.
6. Output:

```json
{"bottleneck": "...", "why": "...", "expected_effect": "...", "ab_signal": "improve|regress|skipped"}
```

7. End with **`CANDIDATE_READY`** only if A/B did not regress (required).

If you cannot test the hypothesis, output `CANNOT_TEST` with JSON `{"status": "cannot_test", "reason": "..."}`.

If you have already been aggressive on both axes and further attempts are stuck (no real dual-axis movement left — whatever absolute number that is for this query), stop:

```
OPTIMIZATION_SATURATED
```

```json
{
  "status": "optimization_saturated",
  "reason": "stuck: recent attempts not moving dual-axis score; no clear remaining redesign",
  "remaining_headroom": "low"
}
```

Do **not** call this early while write-mix is collapsed (≪1×) or while a clear structural/maintenance redesign is still untried. Absolute speedups are query-dependent.

{{benchmark_notes}}

{{constraints}}
