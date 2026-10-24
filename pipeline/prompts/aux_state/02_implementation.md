# Stage 2: Aux-state implementation (stateful module)

Implement the aux-state accelerator for **{{display_name}}** **Q{{query_number}}**.

## Goal

**A correct stateful module with fast reads and cheap maintenance from copy-on-write state, at SF3000.**

1. **Correct first.** `correctness_facts` in the analysis is the semantic contract; the gates must pass. Do not trade correctness, concurrency safety or maintenance semantics for speed.
1. **Build the analysed design, all of it, from the first version.** Each state component lives in the primitive its `state_storage` entry names, the read path follows `composition_rule`, and every write gets the action `mutation_coverage` gives it. A `supported_delta` write is maintained incrementally (a touched-path COW update whose cost follows the touched rows): a rebuild or resync for it is a defect, not a first draft, even when every subset check passes, because at full scale it turns each write into a full scan. Rebuilds exist only where the analysis lists them or the cost model picks one for a refresh-sized batch. If part of the plan cannot be built, keep the rest and call `request_structural_reanalysis` with the evidence; never substitute a simpler design silently.
2. **Low-level execution** on the read, build and maintain paths: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. Guide: `pipeline/prompts/shared/low_level_execution.md`.
3. **State from the COW primitive library.** Reuse a catalogued primitive when it fits (`primitives_used` in the analysis). Otherwise design a bespoke, generic COW primitive for this query template's access pattern (`primitives_proposed`) and publish it with `primitive_publish` **before using it** (additions only, generic, tested). Never hand-roll a private COW container in the module; never deep-copy state per commit. Maintenance cost is proportional to touched data. Guide: `pipeline/prompts/aux_state/shared/cow_primitives.md`.
4. **Aggressive read target: at least 20x faster than the engine's own SQL for the same binding**. Measure it yourself with your own micro-benchmark from the start; below 20x, optimization keeps pushing (structural redesign or low-level work) and cannot stop.
4. **The read path reports the state it visits.** Call `RecordPartitionsRead(units, bytes)` (`accelerator_codegen::aux_state::core`) once per read with the entries/nodes/partitions touched and the bytes read: cheap, compiled in. `run_profile` shows it next to the analysis's `read_work_ratio`, so "the read visits too much state" is measured, not guessed.

**Owns:** the query's module, finalize, layout, tests, benchmark, calibrator, cost-model config and its registrations. **Never touches:** `backends/`, `substrate/`, `cmake/`, other queries, `accelerators/prims/` except through `primitive_publish` (`agent_boundary.md`). **Judged by:** `aux_correctness` and `aux_multi_connection` (you pre-verify both), then the orchestrator's `aux_unit`, `prims_tests`, `aux_cost_model_calibrate`, `aux_cross_accel`, `aux_tsan`, `aux_bench_sweep`.

The test suite (`tests/workload/`, written by the tester and frozen) is the acceptance: run it with `quick_check`, never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md` (the test, the evidence, what is right) and the tester decides.

{{hard_guidelines}}

Pipeline files (`pipeline/`, `substrate/`, `backends/` gates and prompts, `cmake/`) are not yours: edits there are reverted and warned about in tool footers; if the task is blocked on one, call `report_blocker`.

**Stuck?** Call `ask_advisor(question)` when you have been on one failure for more than ~10 minutes, or before you declare that part of the analysed plan cannot be built or a failed check "only the budget remains". A stronger model reads this whole conversation and the files and returns a diagnosis with next steps; a few consults per stage, so say what you tried.

{{codex_tooling}}

{{fast_loop}}

## Implementation contract

```json
{{implementation_context_json}}
```

Full analysis: `{{full_analysis_path}}` · Workload SQL: `{{reference_sql_path}}`

{{implementation_bootstrap}}

## Build the module

- Implement `files_to_create`; the selected candidate is `correctness_facts.state_design.selected_candidate_id`. Every field, mutation rule and read-path behavior matches `correctness_facts`; re-read the analysis and reference modules instead of guessing.
- Implement the **whole** surface: state layout, encode/decode/finalize, maintenance, read path, tests, benchmark, calibrator wrapper. A fast but incomplete or wrong module fails.
- The frozen suite's `suite.yaml` sets the point-write and bulk minimums: the catalog `probe_select`/`build_probe` supports multiple invalidating point writes, bulk coverage matches `bulk_writes`, and each `scale_scenarios` entry runs in the shared differential gate.
- **Register everywhere.** Wire the query into every registration, allowlist, SQL-helper and CMake surface that neighboring aux queries use for host **`{{engine_name}}`**: search how a peer is registered (`registration_checklist.md`). Missing participation shows up as `unsupported shared query` or a missing SQL entrypoint. The pipeline has already scaffolded the query's registration (a module that declines every call, answered by the fallback SQL; the test suite was validated through it): replace that file's registration with the real one in place (same path), keeping the accelerator id and table function.
- **Parallel and scale-safe by construction** (`substrate_primitives.md`, `engine_interfaces.md`, `scale_factor_contract.md`; implement the analysis's `scale_plan`). `Build` runs through `core::PartitionedBuild` and the **scan** role's partitioned scan. Data-proportional work in `Execute`, build and maintenance splits with `ParallelReduce` / `ParallelEach`, with the same result at any cap including 1. Thread caps come only from the substrate's thread budget (`substrate/aux/runtime/thread_budget.hpp`): `ACCEL_AUX_THREADS` for every phase, `ACCEL_AUX_READ_THREADS` / `ACCEL_AUX_BUILD_THREADS` / `ACCEL_AUX_MAINT_THREADS` per phase, never a hard-coded or hardware-derived count; the gates check correctness at 1 and N threads and the bench reports the N-thread speedup (a read the analysis showed output-bound stays one slice); maintenance does its engine lookups first, serially. Size containers from table statistics / `DataScaleRows()`, accumulate and emit sums in 128 bits, keep keys, offsets and counts 64-bit (narrow only with a checked cast). Fill `MaintenanceRequest` honestly and go through `ChooseMaintenance`; the cost-model JSON (`{{cost_model_config}}`, calibrated by the gate) records `calibrated_scale_rows`, and `CalibrationWrites()` includes refresh-shaped writes (bulk, keys spread). Populate `interference_us` in `StageMaintenance` from `CostModelParams.interference_*`.
- Rebuild actions resync state for later hits (never invalidate-only). Irrelevant mutations are NoOp; reads are bounded to the window; reader epoch pin and execution gate follow `read_path_scaffold.md`, `concurrency_contract.md`.
- Read the bootstrap reference paths once, then write. Do not re-explore the repo for layout.

{{backend_api}}
{{accelerator_engine_profile}}
{{engine_host_discovery}}

## Benchmark, calibrator, tests

- `benchmark_q{{query_number}}_aux.cpp`: the full write-frequency sweep per `bench_sweep_contract.md`, ported from a peer's `bench/benchmark_q*_aux.cpp` with the shared sweep and invalidating-probe helpers so write ticks invalidate this query's state.
- `calibrate_q{{query_number}}_aux_cost_model.cpp`: the thin wrapper over the shared calibration helper, in the shape of a peer's. Custom timing code in the calibrator is rejected. `accelerators/{{benchmark_name}}/q{{query_number}}/cost_model.hpp` (written by the scaffold) names the config directory; pass its `AuxCostModelLocation` to the engine's registration.
- The module unit test is shared (`unit_test_contract.md`): write no per-query unit test; make the catalog probe and arguments follow `reference_sql_path` (SQL polarity: `NOT LIKE`, `<>`, anti-joins) and the **Reference SQL predicate contract** in the bootstrap.
- Off-spec parameters, rows and keys are the frozen suite's job (fixed tuples, a fresh random draw each run, every write kind); run the host's params test (plain, `--data-perturb`, `--wide-keys`, `--negative-keys`) when it exists.

## Exit criterion (hard)

Compiling is not enough. Do not end the stage until all of these have **passed** (not merely run):

1. After new module, test, bench or calibrator sources: `cmake -S . -B <worker_build_dir>` once. Build the host's differential binary and gate harnesses with `backends/{{engine_name}}/host.sh` (`build`, `sql`, `run`, `slot`; `query_host_bootstrap.md` when a harness lacks this query).
2. `quick_check` green after every edit. It runs the whole frozen suite at N and 1 threads on the subset and projects each write kind's cost to the full dataset: iterate until it passes with no projected-cost failure (a write that rebuilds the state fails it). The heavy gates below are refused before that, and each then runs once, not as a debugging loop.
3. `run_correctness` / `request_gate` `aux_correctness` (module smoke plus the SF1 differential on the **{{engine_name}}** host). On failure fix the implementation or registration, never the fixtures.
4. `run_multi_connection` (the engine's own harness).
5. `request_gate` `aux_tsan` when the module has `ParallelEach` / `ParallelReduce` / `PartitionedBuild` code: it fails on a data race or a forced-grain mismatch.
6. The scale verification set of `scale_factor_contract.md` ran and passed: the shared differential on the shifted-key and inflated-value databases including writes, at 1 and N read/build/maintenance threads; `ReadAuxExecuteCounts` and the decision log show no unexpected rebuild or fallback. Report the commands and results.
7. The module is complete: the selected candidate is fully implemented; `Execute` applies the same predicates as the SQL fallback; every template parameter is exact for any value including NULL; a write that duplicates a key the query joins or groups on still gets the exact answer (handled, or declined to the SQL fallback while the keys are not unique); no key or offset narrower than 64 bits; decimals exact; writes cost O(touched); the state is COW and built from `prims/`.

If a structural bottleneck cannot be fixed by the viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max 2 per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
