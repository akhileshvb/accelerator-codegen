# Stage: Workload harness bootstrap (once per engine)

You are building the **workload harnesses** of **`{{engine_name}}`**: the query-parameterised test and benchmark programs that every per-query accelerator run of this engine is validated and measured with. They run once per engine, after the `engine_backend` run produced the backend and before the query workers start.

`engine_name` is **required**. Do not default to another engine.

{{hard_guidelines}}

{{codex_tooling}}

{{engine_host_discovery}}

## What you may write

Only these paths. Everything else in `backends/{{engine_name}}/`, every other backend, `substrate/`, `workloads/` and `accelerators/` is read-only to this stage.

- `backends/{{engine_name}}/tests/harness/**` (test sources and headers)
- `backends/{{engine_name}}/bench/**` (bench and calibrate sources)
- `backends/{{engine_name}}/tests/harness.cmake` (declares the targets; `cmake/Backend.cmake` includes it right after `backend.cmake`; use `accel_add_backend_exe({{engine_name}} <target> <sources>)`)

If the backend lacks something the harnesses need (a registry lookup, a thread knob, table rows), append a request `{"benchmark", "query", "scope": "backend", "helper", "reason"}` (no query names in `helper`) to `pipeline/artifacts/backend_additions/{{engine_name}}.jsonl`, say so in your report, and work around it only inside the harnesses.

## Targets (one naming convention for every engine; the gates look these up)

| Target | Source | Run as (through `host.sh run`) |
|---|---|---|
| `{{differential_binary}}` | `{{differential_source}}` | `{{differential_binary}} <query>` |
| `{{multi_conn_target}}` | `backends/{{engine_name}}/tests/harness/{{multi_conn_target}}.cpp` | `{{multi_conn_target}} <query>` |
| `{{cross_accel_target}}` | `backends/{{engine_name}}/tests/harness/{{cross_accel_target}}.cpp` | `{{cross_accel_target}} <query>` |
| `{{harness_bench_target}}` | `backends/{{engine_name}}/bench/{{harness_bench_target}}.cpp` | `{{harness_bench_target}} <query> <write_pct>` (also `--query N`) |
| `{{harness_calibrate_target}}` | `backends/{{engine_name}}/bench/{{harness_calibrate_target}}.cpp` | `{{harness_calibrate_target}} --query <N> <cost_model.json>` |

Helper headers and extra sources go next to them. Every binary answers `--help` with its usage and exit 0 **without touching the database** (the gate smokes each of them that way).

## Workload-generic code

One harness per kind. Each takes the **query number** (and, through the target name, the benchmark) as an argument and finds everything else itself:

- query SQL: `workloads/<bench>/q<N>.sql`; schema: `workloads/<bench>/schema.sql`; seeded parameter instances: `workloads/<bench>/instances/`;
- the accelerator: the **registration** role of `backends/{{engine_name}}/api/README.md` (look the query up in the registry; the queries register themselves from `accelerators/<bench>/q<N>/{{engine_name}}/`);
- parameters: the benchmark's parameter generator with `ACCEL_BENCH_ARG_SEED` (see `eval/tests/tpch_param_templates.hpp`; fresh draws every run, never pinned values).

A harness never names a query, never switches on a query number, never includes a header of `accelerators/<bench>/q<N>/`. The gate runs `scripts/check_backend_query_free.py` on these files. It builds and runs with zero registered queries (then it has nothing to compare: print so, exit 0 for `--help` only).

## What each harness does

- **Differential** (`<query>`): provision nothing; the instance is up and loaded. (1) Read: for the query's parameters, the engine's reference SQL and the accelerated path return the same ordered rows (floats tie-aware, see `eval/harness/float_order_ties.hpp`, `eval/tests/test_support/result_compare.hpp`). (2) Write maintenance: apply writes of every kind the benchmark has (for TPC-H the RF1/RF2 refresh sets from `eval/tests/tpch_fuzz_writes.hpp`, plus point updates that invalidate the query) as plain SQL on a **separate connection**, compare after each, then undo; also a bulk write. Honour `ACCEL_ADV_MAX_PROBES`, `ACCEL_ADV_MIN_PROBES`, `ACCEL_ADV_MIN_CHUNKS`, `ACCEL_ADV_REQUIRE_BULK` (the aux gate sets them from the suite's `suite.yaml`) and `ACCEL_FORCE_MAINTENANCE_ACTION` where the backend has the knob. This one binary is what the `aux_correctness` gate runs per query (the 22-query sweep and the RF differential are that gate over the pack's queries).
- **Test suite**: when `ACCEL_SUITE_DIR` is set, the differential also runs the frozen suite on a copy of the database and, with `ACCEL_SUITE_MODE=validate`, validates it without an accelerator; exact environment and report in `pipeline/prompts/engine_backend/host_contract.md` ("Test suite").
- **Multi-connection stress** (`<query>`): a writer and several readers on separate connections for `ACCEL_MULTI_CONN_STRESS_SEC` seconds (`ACCEL_MULTI_CONN_TARGET_READERS`, `ACCEL_MULTI_CONN_BACKGROUND_READERS`, `ACCEL_MULTI_CONN_TEST_QUERIES`); every read equals the reference at its snapshot.
- **Cross-accelerator** (`<query>`, `ACCEL_CROSS_ACCEL_TEST_QUERIES`): the query's accelerator next to the other registered accelerators of the engine: writes to one table invalidate exactly the accelerators that read it, answers stay exact, a failure of one does not take down the others. With a single registered query, run the isolation checks that make sense and pass.
- **Bench** (`<query> <write_pct>`): read-only speedup of the accelerator vs the reference SQL, and a write mix at `write_pct`; print `read_only_query_speedup,<x>` and `write_mix_workload_speedup,<x>` lines (the baseline gate parses them). Environment: `ACCEL_BENCH_WARMUP`, `ACCEL_BENCH_QUERIES`, `ACCEL_BENCH_REF_SAMPLES`, `ACCEL_BENCH_BASE_WRITE_SAMPLES`, `ACCEL_AUX_WRITE_FREQUENCIES`, `ACCEL_AUX_WRITE_INTERVALS`, `ACCEL_Q<N>_WRITE_FREQUENCY_PCT`, `ACCEL_AUX_BENCHMARK`. Parse the command line with `eval/bench/driver/bench_cli.hpp`.
- **Calibrate** (`--query <N> <cost_model.json>`): measure the query's maintenance and interference costs on this engine and write the cost model JSON (`substrate/aux/core/aux_calibration.hpp` is the calibration core; the file must have `interference_delta_per_query_us > 0` and `interference_rebuild_per_query_us > 0`). Environment: `ACCEL_INTERFERENCE_CALIB_SEC`, `ACCEL_INTERFERENCE_ISOLATED_SAMPLES`.

## How the harnesses reach the engine

Only through `backends/{{engine_name}}/api/` (index: `backends/{{engine_name}}/api/README.md`) and the environment `host.sh` provides: `host.sh up` wrote `$ACCEL_ENGINE_STATE_DIR/env` (`KEY=VALUE` lines: the database or connection settings), and `host.sh run <target> [args]` applies it, so the binary reads its connection from the environment. Never hard-code a path, port, container or tool of the server. SQL connections, mutation, the engine's SQL client and comparison helpers come from the api; do not reimplement runtime flow (commit ordering, catch-up, builds, thread pools): substrate owns it (`substrate/aux/runtime/`). Size threads from the backend's thread budget, never `hardware_concurrency`.

Engine-neutral pieces to reuse (they are in the clean tree): `eval/bench/driver/bench_cli.hpp` (CLI and env helpers of the drivers), `eval/tests/test_support/` (`db_paths.hpp`, `result_compare.hpp`, `benchmark_paired_timing.hpp`, `interference_calibration.hpp`, `toy_build_state.hpp`), `eval/harness/float_order_ties.hpp`, `eval/harness/slot_gates.hpp` (a model of an engine adapter for the generic scenarios), `eval/tests/tpch_param_templates.hpp`, `eval/tests/tpch_fuzz_writes.hpp`. The reference tree's engine-bound harness code (`eval/harness/differential.*`, `cost_calibration.*`, `tpch_rf.*`) is not in a clean tree; peer backends' `tests/harness/` and `bench/` show a finished set when they exist.

## Ordered plan

1. Read `backends/{{engine_name}}/api/README.md`, `host.sh` (`info`), the registration role's headers, and the files above. Check `backends/{{engine_name}}/backend.cmake` for the engine SDK interface target `accel_backend_{{engine_name}}`.
2. Write `tests/harness.cmake` declaring all five targets, then the **differential** first, then the rest.
3. `request_gate` **`host_bootstrap`** after the differential builds, and again as you add the other harnesses. Build only through `backends/{{engine_name}}/host.sh build <targets>` (never a bare cmake/make).
4. Fix from the gate's `log_tail` until it passes.

## Exit criterion

`request_gate` **`host_bootstrap`** passes: every target in the table above built through `host.sh build`, the differential ran on a registered query (or answered `--help` when none is registered yet), every other target answered `--help`, and the harness files name no query. A critic then reads the harnesses and may send you back for stubs.
