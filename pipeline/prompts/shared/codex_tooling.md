# Codex exec tooling

You run under **Codex** with native edit/search/shell tools **plus** accelerator-pipeline MCP tools (`accel_pipeline`).

## Pack isolation (non-negotiable)

Concurrent pack workers use **per-query git worktrees** and private build dirs.

- **Edit only this query's owned files** (listed in the task). Never create/modify/delete another `qN` aux/module/test/cost-model — not even to "fix" a peer compile error.
- Peer compile noise is not your bug under worktree isolation. Do **not** `git restore` / `cp` / Python-rewrite foreign `accelerators/<bench>/q*/**`.
- Always build/test against **this worker's build dir** (`build-release-<worker_id>` / `$ACCEL_PIPELINE_BUILD_DIR`). Never use shared `build-release/`.
- Broad grepping/editing of other pack queries is not OK.

## Use Codex native tools for

- Reading and searching code
- Editing **owned** sources (prefer shell/`sed`/`python` edits when more reliable than Codex's patch tool)
- **Own shell commands and throwaway scripts** under `/tmp` or owned paths — inspect dumps, parse bench output, compare A/B CSVs, bisect hot paths, etc.
- Ad-hoc local debug / rebuild / ctest when MCP is insufficient (**worker build dir only**)

Do not wait for a perfect MCP tool if a short script answers the measurement question faster. Prefer MCP for official digests / A/B / gates when available. For **engine_backend impl_*** stages, MCP `run_engine_build` / `run_layer_gate` / `request_gate` **is** the pass bar — shell cmake/`docker exec` is debug only.

## Use MCP (`accel_pipeline`) for measurement — mandatory for opt/impl feedback

Call these **by tool name** on server `accel_pipeline`. Do **not** use `list_mcp_resources` (resources are empty; tools are not resources).

| Tool | Role |
|------|------|
| `get_profile_digest` | Scoreboard (`best_valid_score`), freshness, phase_breakdown, work counters, store diagnostics |
| `get_optimization_evidence` | Bottleneck evidence + runtime markdown |
| `get_store_diagnostics` | Aux store diagnostics (`partitions_read`, `accel_hit_*`, …) |
| `run_profile` | Remeasure CURRENT tree (aux: quick instrumented bench). Clears stale digests |
| `quick_check` | Seconds-level check after **every edit**: incremental build, the query's own tests, the differential on the small test subset. Pass/fail with seconds per phase; a failure names the step and how to reproduce it. `sanitizer="thread"\|"address"` with `threads=N`: the same in an instrumented build of its own (incremental), with sanitizer findings that have a frame in the query directory; run it after touching concurrent code |
| `run_correctness` / `run_multi_connection` / `run_baseline` / `run_fairness` / `request_gate` | Acceptance gates (minutes; impl: correctness + multi-conn). `request_gate(gate, background=True)` starts aux_correctness / aux_unit / prims_tests / aux_multi_connection / aux_cross_accel in their own process and returns at once |
| `gate_status` | Read a background gate once, when you need the answer (never poll it in a loop). `stale: true` means you edited after it ran: the orchestrator reruns it at stage exit |
| `begin_ab_trial(trial_id, hypothesis, hot_spot, expected_gain, strategy)` / `run_quick_benchmark` / `end_ab_trial` | Exploratory A/B vs **best_valid_score** — **honor the signal**. `run_quick_benchmark` gives direction in seconds; `acceptance=True` is the full tier `end_ab_trial(keep=True)` needs |
| `request_structural_reanalysis` | Aux structural redesign escape hatch |
| `primitive_publish` | Add a generic primitive to the shared `accelerators/prims/` library (stage files under `pipeline/prims_staging/`; additions only) |
| `run_pipeline_command` | Allowlisted cmake/ctest only |

If an MCP call fails, retry once; then fall back to Codex shell for build/ctest only — still prefer MCP for digests / A/B / correctness when the server is up.

Every tool result may end with a wall-budget notice. It is real: when it says to wrap up, checkpoint your state (leave the tree building and the last passing version in place) and finish the step you are on.

## Scoreboard and freshness

`scoreboard.best_valid_score` is the only keep/regress target. If `freshness.stale` is true (after any accept or restore), call `run_profile` before hypothesizing.

The loop itself (edit, `quick_check`, direction bench, acceptance bench, gates in the background) is in the stage prompt's "How to work" block. Custom profiling lines: `profiling_conventions.md`.

## Optimization worker: do not exit without a marker

Before ending the Codex task you **must** print exactly one of:

- Hypothesis JSON **and** the line `CANDIDATE_READY`, or
- `CANNOT_TEST` with JSON `{"status": "cannot_test", "reason": "..."}`

Exiting after edits/tests without that marker **fails the attempt** and discards the work. Finish with the marker even if exploratory bench was skipped — but skipping A/B after a measurable edit is discouraged.

## Implementation stage (mandatory mid-loop gates)

For **implementation**, do **not** finish until the acceptance gates pass (keep iterating — a failed gate is a fix signal, not a stop). Between edits use `quick_check` (see "Inner loop"); the gates below are the acceptance run.

### Query / aux accelerators

1. Build the smoke or unit target for this query (and the engine's `differential_binary` for aux).
2. Once `quick_check` passes, call **`run_correctness`** (maps to `aux_correctness` for stateful, else `correctness`); `request_gate(..., background=True)` if you have more to do meanwhile.
3. On fail: read `failure_summary`, fix implementation / registration, get `quick_check` green again, then re-run the gate until `status=pass`.
4. For **aux-state**, also call MCP **`run_multi_connection`** (or `request_gate` with `aux_multi_connection`). On fail (often missing TF / shared allowlist wiring), **register the query everywhere** peer queries are registered, rebuild, retry until pass.
5. Only after both required gates report `status=pass` (and are not `stale`) end the stage.

`aux_correctness` includes **smoke fixtures + SF1 SQL-vs-aux differential**. Compile-only or smoke-only handoff is not acceptable. Orchestrator post-stage gates still re-check.

Stateful modules: `pipeline/prompts/aux_state/shared/unit_test_contract.md` (catalog probe and arguments) and `registration_checklist.md` (register everywhere).

### Engine backend (`workload_kind: engine_backend`)

Implementation is **sequential layers** (`layers.md`): `capture` → `epochs` →
`scan` → `runtime` → `host` (host skipped when not required).

1. Implement **only this layer’s** objects + `backends/<engine>/tests/test_<engine>_<layer>.cpp`.
2. Wire CMake so this layer’s target actually builds.
3. **Steer with MCP tools on server `accel_pipeline`** (these are **tools**, not
   resources — do **not** `list_mcp_resources`):
   - `run_engine_build()` after CMake / adapter TU changes
   - `run_layer_gate()` after the harness should run (maps `impl_<layer>` → `engine_<layer>`)
   - or `request_gate` with `engine_build` then this layer’s gate
     (`engine_capture`, `engine_epochs`, `engine_scan`, `engine_runtime`,
     `engine_host`)
4. On fail: read `failure_summary` / `log_tail`, fix, call the MCP tool again.
   **Do not** treat host `cmake`, `ninja`, or `docker exec` of the harness as
   the pass bar — MCP is the official in-container (or host_cmake) gate.
5. Do **not** author `test_<engine>_adversarial.cpp` during implementation.
6. Do not implement later layers' contracts (this layer's own wiring in their files is yours), and do not finish on cmake-only success or a question.
7. **Prior-layer patch (narrow):** if this layer is blocked by a real bug or
   design flaw in an earlier layer, make the smallest fix there, keep that
   prior harness green (add the missing assert if needed), and `request_gate`
   that prior gate before continuing. Do not rewrite prior layers for style.

### Engine backend validation stage

1. Read the adapter + existing **layer** harnesses.
2. Author unseen `backends/<engine>/tests/test_<engine>_adversarial.cpp` (≥4 new cases).
3. Loop `request_gate` for layer gates (regression) and **`engine_adversarial`** until pass.
4. Fix adapter bugs found by adversarial tests; never weaken asserts to greenwash.

## Engine backend layer critique

You are a **read-only critic**. Do **not** edit adapter, harness, or CMake sources
(native Codex tools included). Write only
`pipeline/artifacts/engines/<engine>/layer_critique_<layer>.json` and the
matching `.md`; write them with a shell heredoc.
A green layer gate is not enough — fail on untested contract paths.

**Done = files on disk.** Last action: heredoc-write the JSON then the `.md`,
then stop. Do not explore later-layer trees. If you have enough to judge,
write immediately (`cat > pipeline/artifacts/engines/<engine>/layer_critique_<layer>.json <<'EOF'`).
The orchestrator fails the stage if that JSON is missing.

## Engine backend architecture audit

Same write rule: last action is `architecture_audit.json` + `.md` under
`pipeline/artifacts/engines/<engine>/`. Do not end without those files.
