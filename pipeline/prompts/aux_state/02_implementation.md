# Stage 2: Aux-state implementation

Implement the aux-state accelerator for **{{display_name}}** **Q{{query_number}}**.

{{hard_guidelines}}

{{codex_tooling}}

## Primary goal: correct aux state first

**Correctness is non-negotiable.** Your job is to implement aux state that faithfully matches the semantic contract in `correctness_facts` and passes all official gates. Speed and micro-optimizations are secondary: do not sacrifice correctness, concurrency safety, or maintenance semantics for performance.

Honor frozen adversarial write scenarios when present at `{{artifacts_path}}/adversarial_write_scenarios.json`: catalog `probe_select`/`build_probe` must support multiple invalidating point writes, and RF/bulk coverage must match `bulk_writes`, and each `scale_scenarios` entry is run — the shared differential gate runs multi-probe + RF adversarial checks after smoke.

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

- **Primitives first.** Before writing state code, read `prims/README.md` and use existing primitives (`primitives_used` in the analysis). For each `primitives_proposed` entry (or any generic container you discover you need), **publish it before using it**: write the generic header (structured PURPOSE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN comment, no workload words), the README row (+ "Which one to use" line) and randomized reference tests (std:: model, thousands of ops, copy/mutate-copy/old-version-unchanged checks) as full files under `pipeline/prims_staging/<repo-relative path>`, then call `primitive_publish(files, rationale)`; fix what it reports and retry until `ok`. Prefer a **new** primitive over modifying an existing one (existing headers may only gain lines). Never hand-roll a private COW container in the module, and never deep-copy the state per commit.
- **Parallel and scale-safe by construction** (`substrate_primitives.md`, `engine_interfaces.md` thread rules, `scale_factor_contract.md`; implement the analysis's `scale_plan`). `Build` runs through `core::PartitionedBuild` and the engine's `ScanPartitioned` (no single streaming scan into one state); data-proportional work in `Execute` and in maintenance is split with `ParallelReduce` / `ParallelEach` and gives the same result at any cap, including 1; maintenance does its engine lookups first, serially. Size containers from table statistics / `DataScaleRows()`, accumulate and emit sums in 128-bit, keep every key, offset and count 64-bit (narrow only with a checked cast), and add no size guard that rebuilds. Fill `MaintenanceRequest` honestly and go through `ChooseMaintenance`; the cost-model JSON records `calibrated_scale_rows`, and `CalibrationWrites()` includes refresh-shaped writes (bulk, keys spread), not only tiny updates.
- Use `read_reference_files` once on bootstrap reference paths, then **write** — do not re-explore the repo for layout.
- After the structure is correct and mid-stage gates pass, tighten hot paths (encoding, lookups, maintenance scheduling) without changing observable behavior.

## Benchmark & concurrency

- `benchmark_q{{query_number}}_aux.cpp`: full write-freq sweep harness per `bench_sweep_contract.md` (port from `accelerators/tpch/q4/bench/benchmark_q4_aux.cpp` or `accelerators/tpch/q2/bench/benchmark_q2_aux.cpp`). Prefer `RunConfiguredMultiSweep` / `CollectInvalidatingSharedProbes` so write ticks actually invalidate this query's aux.
- Concurrency: irrelevant mutations → NoOp; bounded window reads; per-accelerator epochs; reader epoch pin + execution gate (`read_path_scaffold.md`).

**Post-stage gates** (orchestrator still runs these after you finish): `aux_correctness`, `aux_unit`, `prims_tests`, `aux_cost_model_calibrate`, `aux_multi_connection`, `aux_cross_accel`, `aux_bench_sweep`.

You must **pre-verify `aux_correctness` and `aux_multi_connection` yourself** before ending (see exit criterion below). Other post-stage gates remain orchestrator-owned.

Populate `interference_us` in `StageMaintenance` using calibrated `CostModelParams.interference_*` fields; `{{cost_model_config}}` is calibrated by the gate. `accelerators/{{benchmark_name}}/q{{query_number}}/cost_model.hpp` (scaffold writes it) names the query's config directory; pass its `AuxCostModelLocation` to the engine's registration (DuckDB: last field of the `SharedQueryCatalogEntry` in `duckdb/aux/register.cpp`; Postgres / MySQL: the `Runtime` constructor) — peer: `accelerators/tpch/q6/`.

`calibrate_q{{query_number}}_aux_cost_model.cpp` must be a **5-line wrapper** calling `CalibrateAndWriteCostModel({{query_number}}, argc, argv)` from `calibrate_aux_cost_model_common.hpp` (copy `calibrate_q18_aux_cost_model.cpp`). Custom timing/benchmark code in the calibrator is rejected by the gate.

## Exit criterion (hard) — do not finish until mid-stage gates pass

Compiling is not enough. **Ending this stage without passing `aux_correctness` and `aux_multi_connection` is a failure** (same for every `engine_name`). Keep fixing and re-running until required gates pass.

Follow `pipeline/prompts/aux_state/shared/unit_test_contract.md` and the **Reference SQL predicate contract** in the bootstrap. The module unit test is shared (`eval/tests/test_tpch_module.cpp`, ModuleRuntime-based): write no per-query test, make the catalog probe/arguments follow `reference_sql_path` (SQL polarity: `NOT LIKE`, `<>`, anti-joins).

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
6. **ThreadSanitizer (required when the module has parallel read/build/maintenance code, i.e. any `ParallelEach` / `ParallelReduce` / `PartitionedBuild`):** `request_gate` `aux_tsan` until pass; it fails on a data race in substrate/backends/accelerators code or a forced-grain differential mismatch.
7. Broader unit coverage / bench targets when listed in post-stage gates.

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
8. **Generality (`hard_guidelines.md` §4):** every template parameter is answered exactly for any value incl. NULL (values not precomputed use a fast exact path, never a per-call rebuild); off-spec data handled; no key/offset/id packing narrower than 64-bit; no node-based or per-row-string maps; decimals exact; writes cost O(touched), no `RebuildAll` for routine writes. State is versioned copy-on-write and built from `prims/` primitives chosen via the "Which one to use" guide in `prims/README.md` (a missing generic primitive is published with `primitive_publish` first; no private COW container, no per-commit deep copy).
9. Unit/differential tests include off-spec parameters (NULL, month-end / Feb 29 dates, partial/mixed-case/empty LIKE patterns, unknown enum values, equal params, out-of-range numbers), off-spec rows, and wide/negative keys. Run the host's `test_<engine>_<bench>_params` (plain, `--data-perturb`, `--wide-keys`, `--negative-keys`) when it exists.
10. **Large scale (`scale_factor_contract.md`):** every `scale_plan.failure_classes` item is implemented as stated; the verification set was run and passed (shared differential on `tpch_shift` and `tpch_big` including writes; the differential at 1 and N read/build/maintenance threads; `ReadAuxExecuteCounts` and the decision log show no unexpected rebuild or fallback). Report the commands and results.
11. **Races:** if the module has parallel read/build/maintenance code, `request_gate` `aux_tsan` has passed (ThreadSanitizer, `ACCEL_AUX_TEST_GRAIN=1`; fails on a race in substrate/backends/accelerators code or a forced-split mismatch).

## Structural re-analysis (rare)

If a structural bottleneck cannot be fixed by viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max **2** per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
