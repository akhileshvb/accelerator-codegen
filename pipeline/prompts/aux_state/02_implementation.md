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
- **Register everywhere:** wire this query into **every** shared registration / allowlist / SQL-helper / CMake surface that neighboring aux queries use. Discover sites by searching how a peer query is registered (see `registration_checklist.md`). Do not hand off with only module sources — missing catalog participation, shared-query allowlists, DuckDB TF registration, or helpers causes `unsupported shared query` / `Table Function … does not exist`.
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

Compiling is not enough. **Ending this stage without passing `aux_correctness` and `aux_multi_connection` is a failure.** Keep fixing and re-running until both pass.

Follow `pipeline/prompts/aux_state/shared/unit_test_contract.md` and the **Reference SQL predicate contract** in the bootstrap. Synthetic fixtures must preserve SQL polarity (`NOT LIKE`, `<>`, anti-joins). Do not invent patterns that contradict `reference_sql_path`.

`aux_correctness` runs **two** checks:

1. Smoke: `ctest -R '^tpch_q{{query_number}}_aux_smoke_test$'`
2. SF1 differential: DuckDB reference SQL vs aux execute (`test_tpch_shared_differential <db> {{query_number}}`)

Both must pass. `aux_multi_connection` exercises DuckDB table-function registration + multi-connection stress for this query.

Required loop before you stop:

1. After creating **new** `test_tpch_q{{query_number}}_aux.cpp` / `benchmark_q{{query_number}}_aux.cpp` / `calibrate_q{{query_number}}_aux_cost_model.cpp`, run **`cmake -S . -B <worker_build_dir>`** once (CMake adds targets on configure). Auto-discovery usually suffices; edit `CMakeLists.txt` only if targets are still missing.
2. Build: `cmake --build <worker_build_dir> --target test_tpch_q{{query_number}}_aux test_tpch_shared_differential -j$(nproc)` (and the bench/calibrator targets once they exist).
3. **Correctness gate (required):**
   - **Codex / MCP:** call `run_correctness` (preferred) or `request_gate` with `aux_correctness`.
   - **Else:** run the smoke ctest **and** `./<worker_build_dir>/test_tpch_shared_differential "$ACCEL_TPCH_DB" {{query_number}}`.
4. If status is fail / non-zero: fix the **implementation** (prefer fixing the kernel over weakening fixtures). Rebuild and re-run step 3 until pass.
5. **Multi-connection gate (required):** MCP `run_multi_connection` (or `request_gate` with `aux_multi_connection`). On fail (often missing TF registration / shared allowlist), finish **register everywhere**, rebuild, retry until pass.
6. Also run broader unit coverage when available: `ctest -R 'q{{query_number}}_'` (exclude unrelated failures outside this query).
7. Build `benchmark_tpch_q{{query_number}}_aux`.

Only after steps 3 **and** 5 report **pass** may you end the stage. The orchestrator will re-run these gates afterward; treat a mid-stage fail as blocking, not advisory.

## Before finishing

Correctness checklist (must pass before you consider the stage done):

1. Selected candidate from analysis is fully implemented — not a stub or simplified variant.
2. Mid-stage `run_correctness` / `aux_correctness` has **passed** (not merely attempted).
3. Mid-stage `run_multi_connection` / `aux_multi_connection` has **passed**.
4. Query is registered on **every** shared surface peer queries use (search to confirm; do not assume module-only is enough).
5. Concurrency contract satisfied (epochs, reader pin, irrelevant mutations → NoOp, bounded window reads).
6. Benchmark and calibrator targets build; calibrator is the 5-line wrapper only.
7. Hot-path `Execute` applies the same query predicates as fallback / SQL (do not filter only in fallback).

## Structural re-analysis (rare)

If a structural bottleneck cannot be fixed by viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max **2** per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
