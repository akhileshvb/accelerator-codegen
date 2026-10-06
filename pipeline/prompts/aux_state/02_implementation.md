# Stage 2: Aux-state implementation

Implement the aux-state accelerator for **{{display_name}}** **Q{{query_number}}**.

{{hard_guidelines}}

{{codex_tooling}}

## Primary goal: correct aux state first

**Correctness is non-negotiable.** Your job is to implement aux state that faithfully matches the semantic contract in `correctness_facts` and passes all official gates. Speed and micro-optimizations are secondary: do not sacrifice correctness, concurrency safety, or maintenance semantics for performance.

Honor frozen adversarial write scenarios when present at `{{artifacts_path}}/adversarial_write_scenarios.json`: catalog `probe_select`/`build_probe` must support multiple invalidating point writes, and RF/bulk coverage must match `bulk_writes` — the shared differential gate runs multi-probe + RF adversarial checks after smoke.

Within a **correct** design, still aim for an efficient implementation:

- Prefer layouts and algorithms that are both semantically right **and** fast on the read path (encode/decode, partition lookup, maintenance scheduling).
- Avoid obviously wasteful structures when a correct alternative is equally viable (e.g. redundant copies, unbounded scans, per-row heap traffic where a flat/soa layout is correct).
- Do **not** skip required scaffolding (tests, benchmark harness, cost-model wrapper, concurrency contracts) to chase speed — gates will reject incomplete work.
- Rebuild actions must **resync** state for later hits (not invalidate-only).

**Priority order:** (1) semantic correctness + gate pass, (2) concurrency/maintenance correctness, (3) read-path efficiency, (4) write-path / maintenance efficiency.

## Implementation contract

```json
{{implementation_context_json}}
```

Full analysis: `{{full_analysis_path}}` · Workload SQL: `{{reference_sql_path}}`

{{implementation_bootstrap}}

## Instructions

- Implement `files_to_create`; selected candidate = `correctness_facts.state_design.selected_candidate_id`.
- **`correctness_facts` is the semantic contract** — every field choice, mutation rule, and read-path behavior must match it. When unsure, re-read analysis/bootstrap and reference modules; do not guess.
- Implement the **full** aux-state surface: state layout, encode/decode/finalize, maintenance, read path, tests, benchmark, and calibrator wrapper. A fast but incomplete or semantically wrong implementation is a failure.
- **Register everywhere:** wire this query into **every** shared registration / allowlist / SQL-helper / CMake surface that neighboring aux queries use for host **`{{engine_name}}`**. Discover sites by searching how a peer query is registered (see `registration_checklist.md`). Do not hand off with only module sources — missing catalog participation, shared-query allowlists, host TF/UDF registration, or helpers causes `unsupported shared query` / missing SQL entrypoint.

{{engine_host_discovery}}

- Use `read_reference_files` once on bootstrap reference paths, then **write** — do not re-explore the repo for layout.
- After the structure is correct and mid-stage gates pass, tighten hot paths (encoding, lookups, maintenance scheduling) without changing observable behavior.

## Benchmark & concurrency

- `benchmark_q{{query_number}}_aux.cpp`: full write-freq sweep harness per `bench_sweep_contract.md` (port from `benchmark_q14_aux.cpp` or `benchmark_q1_aux.cpp`). Prefer `RunConfiguredMultiSweep` / `CollectInvalidatingSharedProbes` so write ticks actually invalidate this query's aux.
- Concurrency: irrelevant mutations → NoOp; bounded window reads; per-accelerator epochs; reader epoch pin + execution gate (`read_path_scaffold.md`).

**Post-stage gates** (orchestrator still runs these after you finish): `aux_correctness`, `aux_unit`, `aux_cost_model_calibrate`, `aux_multi_connection`, `aux_cross_accel`, `aux_bench_sweep`.

You must **pre-verify `aux_correctness` and `aux_multi_connection` yourself** before ending (see exit criterion below). Other post-stage gates remain orchestrator-owned.

Populate `interference_us` in `StageMaintenance` using calibrated `CostModelParams.interference_*` fields; `config/q{{query_number}}_aux_cost_model.json` is calibrated by the gate.

`calibrate_q{{query_number}}_aux_cost_model.cpp` must be a **5-line wrapper** calling `CalibrateAndWriteCostModel({{query_number}}, argc, argv)` from `calibrate_aux_cost_model_common.hpp` (copy `calibrate_q18_aux_cost_model.cpp`). Custom timing/benchmark code in the calibrator is rejected by the gate.

## Exit criterion (hard) — do not finish until mid-stage gates pass

Compiling is not enough. **Ending this stage without passing `aux_correctness` and `aux_multi_connection` is a failure** (same for every `engine_name`). Keep fixing and re-running until required gates pass.

Follow `pipeline/prompts/aux_state/shared/unit_test_contract.md` and the **Reference SQL predicate contract** in the bootstrap. Synthetic fixtures must preserve SQL polarity (`NOT LIKE`, `<>`, anti-joins). Do not invent patterns that contradict `reference_sql_path`.

When `engine_name` ≠ `duckdb`, also follow `pipeline/prompts/shared/query_host_bootstrap.md`: bootstrap SQL entrypoints + differential **and replicate** multi-conn / cross-accel / bench harnesses for this host.

`aux_correctness` on the **engine-selected** host (`{{engine_name}}`):

1. **duckdb:** smoke `ctest -R '^tpch_q{{query_number}}_aux_smoke_test$'` **and** `test_tpch_shared_differential <db> {{query_number}}`
2. **other engines:** `test_{{engine_name}}_tpch_differential {{query_number}}` (create harness + host SQL entrypoints if absent); optional smoke fixtures once the host pattern exists.

`aux_multi_connection` / `aux_cross_accel` / other post-stage gates run for **every** engine. Non-duckdb uses `test_{{engine_name}}_…` ctest names and `ACCEL_ENGINE_NAME` — replicate the duckdb harness; do not skip.

Required loop before you stop:

1. After creating **new** module/test/bench/calibrator sources, run **`cmake -S . -B <worker_build_dir>`** once when targets are new.
2. Build the **engine-selected** differential binary and host gate harnesses.
3. **Correctness gate (required):** MCP `run_correctness` / `request_gate` `aux_correctness` until pass.
4. If fail: fix implementation (prefer kernel/host registration over weakening fixtures); rebuild; retry.
5. **Multi-connection (required):** MCP `run_multi_connection` until pass (engine-specific ctest when not duckdb).
6. Broader unit coverage / bench targets when listed in post-stage gates.

Only after required mid-stage gates report **pass** may you end the stage.

## Before finishing

Correctness checklist (must pass before you consider the stage done):

1. Selected candidate from analysis is fully implemented — not a stub or simplified variant.
2. Mid-stage `run_correctness` / `aux_correctness` has **passed** (not merely attempted).
3. Mid-stage `run_multi_connection` / `aux_multi_connection` has **passed** (engine-native harness when not duckdb).
4. Query is registered on **every** shared surface peer queries use for this host (search to confirm).
5. Concurrency contract satisfied (epochs, reader pin, irrelevant mutations → NoOp, bounded window reads).
6. Benchmark and calibrator targets build when listed in post-stage gates; calibrator is the 5-line wrapper only when using the shared calibrator path.
7. Hot-path `Execute` applies the same query predicates as fallback / SQL (do not filter only in fallback).
8. **Generality (`hard_guidelines.md` §4):** every template parameter is answered exactly for any value incl. NULL (values not precomputed use a fast exact path, never a per-call rebuild); off-spec data handled; no key/offset/id packing narrower than 64-bit; no node-based or per-row-string maps; decimals exact; writes cost O(touched), no `RebuildAll` for routine writes. State is built from `core/` primitives chosen via the "Which one to use" guide in `core/README.md`.
9. Unit/differential tests include off-spec parameters (NULL, month-end / Feb 29 dates, partial/mixed-case/empty LIKE patterns, unknown enum values, equal params, out-of-range numbers), off-spec rows, and wide/negative keys. Run the host's `test_<engine>_tpch_params` (plain, `--data-perturb`, `--wide-keys`, `--negative-keys`) when it exists.

## Structural re-analysis (rare)

If a structural bottleneck cannot be fixed by viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max **2** per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
