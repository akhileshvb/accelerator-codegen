# Stage 2: implement the aux-state accelerator for **{{display_name}}** **Q{{query_number}}**

Build the design in the analysis below as a correct, fast, parallel, thread-safe module with copy-on-write state, sized for SF3000.

## The first version is the analysed design, all of it

- **State:** each component lives in the primitive its `state_storage` entry names. Reuse catalogued primitives (`primitives_used`); publish each proposed one with `primitive_publish` before using it (generic, tested, additions only; `cow_primitives.md`). No private COW containers, no deep copy of state per commit.
- **Reads:** follow `composition_rule`, apply the same predicates as the SQL fallback, and answer every value of every parameter exactly (NULL included). Call `RecordPartitionsRead(units, bytes)` once per read with what it visited.
- **Writes:** every write gets the action `mutation_coverage` gives it. A `supported_delta` write is a touched-path COW update whose cost follows the touched rows. A rebuild or resync for it is a defect, not a first draft, even when the subset checks pass: at full scale it makes every write a full scan. Rebuilds exist only where the analysis lists them or the cost model (`ChooseMaintenance`, honest `MaintenanceRequest`) picks one for a refresh-sized batch. A write that duplicates a key the query joins or groups on gets the exact answer (handled, or SQL fallback while keys are not unique).
- **Parallel and thread-safe:** build through `core::PartitionedBuild` and the scan role's partitioned scan; data-proportional work in `Execute`, build and maintenance splits with `ParallelReduce` / `ParallelEach`, with identical results at any thread count including 1. Thread caps come only from the substrate's thread budget (`ACCEL_AUX_THREADS`, and `ACCEL_AUX_READ_THREADS` / `_BUILD_THREADS` / `_MAINT_THREADS` per phase); never a hard-coded or hardware-derived count. Maintenance does its engine lookups first, serially. A serial build or refresh is a defect.
- **Scale:** containers sized from table statistics / `DataScaleRows()`, 64-bit keys, offsets and counts (narrow only with a checked cast), sums in 128 bits, exact decimals (`scale_factor_contract.md`).
- **Low level:** SIMD inner loops with a scalar fallback, cache-conscious layout, branch-free predicates, prefetch, no per-row allocation (`low_level_execution.md`). Target: reads at least 20x faster than the engine's own SQL for the same binding.
- If part of the plan cannot be built, keep the rest and call `request_structural_reanalysis` with the evidence (`ask_advisor` first). Never substitute a simpler design silently.

## Work like this

1. **Write a small engine-free oracle test first** (`accelerators/{{benchmark_name}}/q{{query_number}}/tests/agent_oracle.cpp`): random rows and random commits (inserts, deletes, updates, key moves, duplicates) applied to your state and to a brute-force answer, compared after each commit. It runs in seconds and catches maintenance bugs long before a gate.
2. **`quick_check` after every edit** (pass the oracle as a target). It runs the module's tests and the whole frozen suite on the small subset at N and 1 threads, and reports each write's maintenance action and projected full-scale cost, build time and thread speedup, and read time against the engine's SQL. Fix what it reports first: a rebuild where the analysis is incremental, a build that does not scale, a slow read.
3. **`quick_check(sanitizer="thread", threads=N)`** after touching concurrent code: seconds, on a tiny database.
4. **`run_profile`** when a number is off: a short perf profile of the read and write paths with the top hot spots.
5. **Heavy checks run once, at stage exit, started by the orchestrator:** the full-size differential, stress, TSAN and benchmark. Do not run full-size binaries from the shell. One early `request_gate(<gate>, background=True)` after a green `quick_check` is fine; read it once with `gate_status`.
6. **Stuck on one failure for ~10 minutes,** or about to call part of the plan unbuildable: `ask_advisor`.

The frozen test suite (`tests/workload/`) is the acceptance. Never edit it; a test you can show is wrong goes to `{{artifacts_path}}/test_suite_change_request.md` (the test, the evidence, what is right).

**You own** the query's module, finalize, layout, tests, benchmark, calibrator, cost-model config and registrations. **Not yours:** `pipeline/`, `substrate/`, `backends/`, `cmake/`, other queries, and `accelerators/prims/` except through `primitive_publish` (`agent_boundary.md`). Edits there are reverted; if blocked on one, `report_blocker`.

{{hard_guidelines}}

{{fast_loop}}

## Design contract

```json
{{implementation_context_json}}
```

Full analysis: `{{full_analysis_path}}` (re-read the part you are implementing) · Workload SQL: `{{reference_sql_path}}`

{{implementation_bootstrap}}

## Wiring

- **Register everywhere** a peer aux query is registered for host `{{engine_name}}` (`registration_checklist.md`): replace the scaffolded declining registration in place, keeping the accelerator id and table function.
- **Benchmark** `benchmark_q{{query_number}}_aux.cpp`: the write-frequency sweep per `bench_sweep_contract.md`, from a peer's, with the shared sweep and invalidating-probe helpers.
- **Calibrator** `calibrate_q{{query_number}}_aux_cost_model.cpp`: the thin wrapper over the shared calibration helper; `accelerators/{{benchmark_name}}/q{{query_number}}/cost_model.hpp` names the config directory (`{{cost_model_config}}`). Populate `interference_us` in `StageMaintenance` from `CostModelParams.interference_*`.
- The module unit test is shared (`unit_test_contract.md`): no per-query unit test; the catalog probe and arguments follow the reference SQL predicate contract above.
- Build harnesses with `backends/{{engine_name}}/host.sh` (`build`, `sql`, `run`; `query_host_bootstrap.md` when a harness lacks this query).

{{backend_api}}
{{accelerator_engine_profile}}
{{engine_host_discovery}}

## Done when

`quick_check` (plain and thread-sanitized) is green with no rebuild-like write and a scaling build, your oracle test passes, the scale verification set of `scale_factor_contract.md` ran (shifted-key and inflated-value databases with writes, at 1 and N threads), and every item above is implemented. Then end your turn: the orchestrator runs the exit checks and sends you anything that fails.

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
