# Codex exec tooling

You run under **Codex** with native edit/search/shell tools **plus** accelerator-pipeline MCP tools (`accel_pipeline`).

## Use Codex native tools for

- Reading and searching code
- Editing sources (prefer shell/`sed`/`python` edits when more reliable than `apply_patch`)
- **Own shell commands and throwaway scripts** — you are free to write/run bash, Python, awk, one-off helpers under `/tmp` or the repo to inspect dumps, parse bench output, compare A/B CSVs, bisect hot paths, etc.
- Ad-hoc local debug / rebuild / ctest when MCP is insufficient

Do not wait for a perfect MCP tool if a short script answers the measurement question faster. Prefer MCP for official digests / A/B / gates when available, but **shell autonomy is expected**.

## Use MCP (`accel_pipeline`) for measurement — mandatory for opt/impl feedback

Call these **by tool name** on server `accel_pipeline`. Do **not** use `list_mcp_resources` (resources are empty; tools are not resources).

| Tool | Role |
|------|------|
| `get_profile_digest` | Scoreboard (`best_valid_score`), freshness, phase_breakdown, work counters, store diagnostics |
| `get_optimization_evidence` | Bottleneck evidence + runtime markdown |
| `get_store_diagnostics` | Aux store diagnostics (`partitions_read`, `accel_hit_*`, …) |
| `run_profile` | Remeasure CURRENT tree (aux: quick instrumented bench). Clears stale digests |
| `run_correctness` / `run_baseline` / `run_fairness` / `request_gate` | Mid-loop gates |
| `begin_ab_trial` / `run_quick_benchmark` / `end_ab_trial` | Exploratory A/B vs **best_valid_score** — **honor the signal** |
| `request_structural_reanalysis` | Aux structural redesign escape hatch |
| `record_optimization_trap` | Optional: add a short recurring anti-pattern to shared negative memory (advisory only) |
| `run_pipeline_command` | Allowlisted cmake/ctest only |

If an MCP call fails, retry once; then fall back to Codex shell for build/ctest only — still prefer MCP for digests / A/B / correctness when the server is up.

## Scoreboard + freshness (optimization)

1. Treat `scoreboard.best_valid_score` as the only keep/regress target.
2. If `freshness.stale` is true, call `run_profile` before hypothesizing — prior phase breakdown is invalid.
3. After accept/restore, always remeasure before the next hypothesis.

## Exploratory A/B rules (optimization)

1. `begin_ab_trial` → patch → `run_quick_benchmark` → inspect `ab_signal` / `improvement_pct_vs_reference` / `scoreboard`.
2. `end_ab_trial(keep=true)` only when improved vs best_valid; the server **force-discards** measured regressions.
3. **Never** print `CANDIDATE_READY` for a change that A/B marked `regress` / `forced_discard` / `kept=false`.
4. For large wins: optimize **both** hit-path read latency **and** incremental maintenance (ApplyDelta / local partitions, not RebuildAll-per-write). When digests show a write-mix cliff (10% workload ≪1×) while read is already strong, prioritize maintenance. Keep high-SF speedups. Never cache final query results as aux.

## Custom profiling — modify freely

You may **add, rewrite, split, rename, or remove** temporary profiling instrumentation whenever it helps locate the bottleneck (new phases, finer timers, extra counters, sampling, hierarchical totals). Stay inside editable sources and hard guidelines.

Gate timers on `ACCEL_AUX_CUSTOM_PROFILE` (aux) / `ACCEL_BENCH_PROFILE` (base) — **no-op when unset**. Preferred digest line:

`ACCEL_CUSTOM_PROFILE {"phase":"sort","us":12.3,"emitted_rows":100,"groups":100}`

Require **split timers + work counts** for hypotheses. Digests merge into `phase_breakdown` / `work_counters_mean` after `run_profile` / `run_quick_benchmark`. You may also parse raw bench stdout with your own scripts if the digest is incomplete — then re-run `run_profile` so the scoreboard stays current.

## Workflow

1. Remeasure (`run_profile`) → read scoreboard + phase breakdown (MCP).
2. Implement with Codex native edit/shell.
3. Verify with MCP A/B / correctness.
4. Hand off; the orchestrator still runs authoritative post-stage gates.

## Optimization worker: do not exit without a marker

Before ending the Codex task you **must** print exactly one of:

- Hypothesis JSON **and** the line `CANDIDATE_READY`, or
- `CANNOT_TEST` with JSON `{"status": "cannot_test", "reason": "..."}`

Exiting after edits/tests without that marker **fails the attempt** and discards the work. Finish with the marker even if exploratory bench was skipped — but skipping A/B after a measurable edit is discouraged.

## Implementation stage (mandatory correctness)

For **implementation**, do not finish until the correctness gate passes:

1. Build the smoke / unit target for this query (and `test_tpch_shared_differential` for TPC-H aux).
2. Call MCP **`run_correctness`** (maps to `aux_correctness` for aux-state workloads, else `correctness`).
3. On fail: read the failure summary, fix code, rebuild, re-run `run_correctness`.
4. Only after `status=pass` end the stage.

`aux_correctness` includes **smoke fixtures + SF1 SQL-vs-aux differential**. Compile-only or smoke-only handoff is not acceptable. Orchestrator post-stage gates still re-check.

Follow `pipeline/prompts/aux_state/shared/unit_test_contract.md` when writing synthetic unit fixtures.
