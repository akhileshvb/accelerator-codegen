# Workload harnesses (host bootstrap)

Every engine has the same set of **workload harnesses**, made once by the `host_bootstrap` stage (pack: before the parallel workers; single query: before analysis), after the engine's backend exists. A query run never builds them: it registers its own files under `accelerators/{bench}/q{N}/{{engine_name}}/` and is validated by them.

`engine_name` is **required** (no silent default engine). Names come from the workload `engine_host` block. Stage prompt: `pipeline/prompts/host_bootstrap.md`.

## The set (identical naming for every engine)

| Kind | Target | Source |
|---|---|---|
| differential | `test_{{engine_name}}_<bench>_differential` | `backends/{{engine_name}}/tests/harness/` |
| multi-connection stress | `test_{{engine_name}}_aux_multi_connection_stress` | `backends/{{engine_name}}/tests/harness/` |
| cross-accelerator | `test_{{engine_name}}_cross_accel` | `backends/{{engine_name}}/tests/harness/` |
| bench | `benchmark_{{engine_name}}_<bench>_aux` | `backends/{{engine_name}}/bench/` |
| calibrate | `calibrate_{{engine_name}}_<bench>_aux_cost_model` | `backends/{{engine_name}}/bench/` |
| module test | `test_<bench>_q<N>_aux`, ctest `<bench>_q<N>_aux_smoke_test`: one per query that has an aux module | one shared source in `backends/{{engine_name}}/tests/harness/`, built per query with the benchmark and query number as compile definitions |

Targets are declared in `backends/{{engine_name}}/tests/harness.cmake`. The module test is declared by a glob over `accelerators/*/q*/{{engine_name}}/aux/finalize.cpp` (the discovery `cmake/AuxState.cmake` uses), so a new query gets its test with no edit; it runs the checks of `pipeline/prompts/aux_state/shared/unit_test_contract.md` on the small test database (the `<bench>_test_db` fixture of the root CMake), through the backend api registry. Each takes the query number as an argument and finds the SQL, parameters and the accelerator itself (`workloads/<bench>/q<N>.sql`, the query's frozen suite `accelerators/<bench>/q<N>/tests/workload/params.sql`, the backend api registry by benchmark name): the code names no query and no benchmark. A target per benchmark (`..._<bench>_...`) is one source built with the benchmark name as a compile definition, never a source per benchmark; nothing in the harnesses (sources, `harness.cmake`, argument pools, database paths) is written for one benchmark. CLI, as the gates call it through `host.sh run`: `<target> <query>` (differential, multi-connection, cross-accel), `<bench> --query <N> <write_pct>` (one write-frequency point per run; it prints the CSV of `pipeline/prompts/aux_state/shared/bench_sweep_contract.md`), `<calibrate> --query <N> <cost_model.json>`; every binary answers `--help`.

## Ownership

- The `host_bootstrap` stage writes only `backends/{{engine_name}}/tests/harness/**`, `backends/{{engine_name}}/bench/**` and `backends/{{engine_name}}/tests/harness.cmake`. The rest of the backend belongs to the `engine_backend` run, other engines' backends to theirs, and query runs write only their own directory.
- A query run that finds a defect or a missing helper in the backend or its harnesses **reports** it and does not edit `backends/`: `report_blocker` with `owner` `"harness"` (these workload harnesses) or `"backend"` (the engine backend) records it as a request to that owner, with no query names in its summary. A harness request is fixed by `host_bootstrap` before the query's next check; a backend request backed by a failing check goes to the engine backend's validation before the query's next check. Three more owners stop the run with a report naming them: `"engine"` (the engine itself, its source outside the repository), `"data"` (the workload's data or test cut) and `"pipeline"` (a gate or the orchestrator); `"infra"` is the machine or a provider. Every failing check names its owner (`Owner:` in the failure report); a failure with none is the query's.

## Do / don't

- **Do** reach the engine only through `backends/{{engine_name}}/api/` (index `api/README.md`) and `host.sh` (`up` writes the `env` file the harnesses read, `build`, `run`, `sql`, `slot`). Build with `host.sh build <targets>`, never a bare cmake/make.
- **Do** register the query from its own directory through the **registration** role's registry, passing the cost-model location from `accelerators/{bench}/q{N}/cost_model.hpp`.
- **Do not** reimplement runtime flow (ordering, catch-up, builds, thread pools); it is substrate (`substrate/aux/runtime/`).
- **Do not** invent a second adapter tree, or use another engine's registration or dataset format for this engine.

## Gates

- `host_bootstrap`: builds the five targets through `host.sh build`, checks that `harness.cmake` declares the module test by that glob over every benchmark (`accelerators/*/q*/...`), builds the module test of a registered query, runs the differential on one registered query (or `--help` when none exists yet), `--help` on the others, and checks the harness files name no query and no benchmark.
- Per query: `aux_correctness` (differential, reads and writes including refresh chunks: this is the sweep over the pack's queries), `aux_multi_connection`, `aux_cross_accel`, `aux_cost_model_calibrate`, the bench gates.

## Exit criterion

- **`host_bootstrap` stage:** MCP `request_gate` / `host_bootstrap` passes.
- **Per-query implementation:** the post-implementation gate suite passes for this query. Prefer MCP `run_correctness` / `run_multi_connection` / `request_gate`.
