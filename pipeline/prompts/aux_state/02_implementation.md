# Stage 2: Aux-state implementation

Implement the aux-state accelerator for TPC-H **Q{{query_number}}**.

## Primary goal: correct aux state first

**Correctness is non-negotiable.** Your job is to implement aux state that faithfully matches the semantic contract in `correctness_facts` and passes all official gates. Speed and micro-optimizations are secondary: do not sacrifice correctness, concurrency safety, or maintenance semantics for performance.

Within a **correct** design, still aim for an efficient implementation:

- Prefer layouts and algorithms that are both semantically right **and** fast on the read path (encode/decode, partition lookup, maintenance scheduling).
- Avoid obviously wasteful structures when a correct alternative is equally viable (e.g. redundant copies, unbounded scans, per-row heap traffic where a flat/soa layout is correct).
- Do **not** skip required scaffolding (tests, benchmark harness, cost-model wrapper, concurrency contracts) to chase speed — gates will reject incomplete work.

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
- Use `read_reference_files` once on bootstrap reference paths, then **write** — do not re-explore the repo.
- After the structure is correct and tests pass locally, tighten hot paths (encoding, lookups, maintenance scheduling) without changing observable behavior.

## Benchmark & concurrency

- `benchmark_q{{query_number}}_aux.cpp`: full write-freq sweep harness per `bench_sweep_contract.md` (port from `benchmark_q14_aux.cpp` or `benchmark_q1_aux.cpp`).
- Concurrency: irrelevant mutations → NoOp; bounded window reads; per-accelerator epochs; reader epoch pin + execution gate (`read_path_scaffold.md`).

**Official gates run after this stage** (do not call `request_gate`): `aux_correctness`, `aux_unit`, `aux_cost_model_calibrate`, `aux_multi_connection`, `aux_cross_accel`, `aux_bench_sweep`.

Populate `interference_us` in `StageMaintenance` using calibrated `CostModelParams.interference_*` fields; `config/q{{query_number}}_aux_cost_model.json` is calibrated by the gate.

`calibrate_q{{query_number}}_aux_cost_model.cpp` must be a **5-line wrapper** calling `CalibrateAndWriteCostModel({{query_number}}, argc, argv)` from `calibrate_aux_cost_model_common.hpp` (copy `calibrate_q18_aux_cost_model.cpp`). Custom timing/benchmark code in the calibrator is rejected by the gate.

## Before finishing

Correctness checklist (must pass before you consider the stage done):

1. Selected candidate from analysis is fully implemented — not a stub or simplified variant.
2. `ctest -R tpch_q{{query_number}}_aux` and `ctest -R q{{query_number}}_` pass (correctness + unit behavior).
3. Concurrency contract satisfied (epochs, reader pin, irrelevant mutations → NoOp, bounded window reads).
4. Benchmark and calibrator targets build; calibrator is the 5-line wrapper only.

Build steps:

1. After creating **new** `test_tpch_q{{query_number}}_aux.cpp` / `benchmark_q{{query_number}}_aux.cpp` / `calibrate_q{{query_number}}_aux_cost_model.cpp`, run **`cmake -S . -B build-release`** once (CMake adds targets on configure). Auto-discovery usually suffices; edit `CMakeLists.txt` only if targets are still missing.
2. `run_command`: build + `ctest -R tpch_q{{query_number}}_aux` + `ctest -R q{{query_number}}_`.
3. Build `benchmark_tpch_q{{query_number}}_aux`.

## Structural re-analysis (rare)

If a structural bottleneck cannot be fixed by viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max **2** per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
