# Engine host contract

Engine-agnostic. Every backend provides one executable, `backends/{engine}/host.sh`,
and kept code (gates, orchestrator, scripts, eval drivers) reaches the engine only
through it. Nothing outside `backends/{engine}/` names the engine's server, client,
container, plugin or data layout.

    backends/{engine}/host.sh <verb> [args...]

## Environment (set by the caller)

| variable | meaning |
|---|---|
| `ACCEL_ENGINE_INSTANCE` | instance name, unique per run; container, socket and database names derive from it, so parallel runs never collide. A port is any free one, chosen by `up` and recorded in the `env` file |
| `ACCEL_ENGINE_STATE_DIR` | directory the instance owns (data, logs, `env`); delete it and the instance is gone |
| `ACCEL_BUILD_DIR` | the CMake build directory |
| `ACCEL_DATASET_DIR` | engine-neutral dataset: one `<table>.tbl` (`|`-separated, dbgen format: a trailing `|` per line) per table; a bare empty field is NULL, `""` is the empty string, a value holding `"` is quoted with `""` inside |
| `ACCEL_CMAKE_ARGS` | the arguments the build dir is configured with (generator, build type, this backend's switch, the engine's query sources) |

## Test suite

A query's frozen test suite reaches the engine's differential harness through the environment of `run` (format: `pipeline/prompts/shared/test_suite.md`). When `ACCEL_SUITE_DIR` is set the harness, after its own phases, runs the suite on a writable copy of the database:

| variable | meaning |
|---|---|
| `ACCEL_SUITE_DIR` | the suite directory (`params.sql`, `txns/`, `setup.sql`) |
| `ACCEL_SUITE_PARAMS_FILE` | fresh random tuples of this run, one per line: checked besides `params.sql` |
| `ACCEL_SUITE_SEED` | the run's seed: it seeds every random choice of the run, is printed, and is in the report |
| `ACCEL_SUITE_QUERY_SQL`, `ACCEL_SUITE_FUNCTION`, `ACCEL_SUITE_ARITY` | the path of a file holding the reference SQL (`$n` parameters; read the file, never run the path), the accelerated path's SQL entry, and how many leading tuple values it takes (it derives the rest) |
| `ACCEL_SUITE_TXNS` | optional comma-separated transaction file names: apply only these |
| `ACCEL_SUITE_EVERY` | compare every tuple every N transactions (else two random ones), and after the build and at the end |
| `ACCEL_SUITE_SCHEMA_SQL` | the path of a file of constraints applied before `setup.sql` (NOT NULL columns of the declared schema); empty when there are none |
| `ACCEL_SUITE_REPORT` | where to write the JSON report: `ok`, `seed`, tuples, `checks`, `mismatches`, `write_errors`, `executes`, examples; the write-cost signal of quick_check: `build_seconds` (the module build at `threads`, of which `build_threads` build), `write_costs` (per transaction `txn`, `write_ms` of the commit, `read_ms` of the first accelerated read after it, `maint_ms` the module's maintenance time or -1, `action` the maintenance action the module took: `ApplyDelta`, `RebuildPartition`, `RebuildAll` (also a stale commit rebuilt) or `NoOp`; absent when the engine does not tell, e.g. a module that maintained without asking the policy) and `probe_costs_ms` (the adversarial probes' applies); `phases`: seconds of each phase of the run (`build`, `read_only`, `incremental`, `adversarial`, `suite`, `refresh`, `drift`) |
| `ACCEL_DIFF_PHASES` | comma-separated phases of the differential to run (default all of `read_only`, `incremental`, `adversarial`, `suite`, `refresh`, `drift`). One run builds the module once and shares the database copy, runtime and build among them; writes are undone through the runtime, and `drift`, the run's only from-scratch rebuild, must answer as the maintained state did |
| `ACCEL_SUITE_MODE=validate` | no accelerator: every tuple evaluates, every transaction applies, rolls back and applies again; report per transaction how many tuples' answers it changes (`txn_effects`), the row count per tuple (`tuple_rows`), `errors` |

The validate report, for a suite of three tuples and two transactions:

```json
{"ok": true, "seed": 42, "tuple_rows": [3, 0, 12],
 "txn_effects": [{"txn": "001_insert_orders.sql", "changes_answers_of": 2}, {"txn": "002_delete_items.sql", "changes_answers_of": 0}],
 "errors": []}
```

`tuple_rows[i]` is the row count of the tuple on line i+1 of `params.sql`; `txn` is the file name in `txns/`; `errors`
are strings (a tuple that does not evaluate, a transaction that does not apply).

**Clause `suite-report`**: a differential that ran with `ACCEL_SUITE_DIR` set writes the report at `ACCEL_SUITE_REPORT`.

After the build and after each committed transaction the accelerated answer must equal the reference's at the same snapshot (ties at the order and LIMIT cut are open); a transaction starting `-- rollback` is applied then aborted. A failure exits non-zero naming the seed, tuple and transaction.

## Verbs

| verb | args | does |
|---|---|---|
| `info` | | print one JSON line: `{"engine", "embedded": bool, "verbs": [...], "hit_contract": true, "mounts": {"ro": [abs paths], "rw": [abs paths]}, "docker": bool}`; `hit_contract` is `true` once the backend meets the hit contract below (required: a backend without it fails the `engine_hit_contract` gate); `bench_multi_point: true` says the aux bench runs every write frequency of `ACCEL_AUX_WRITE_FREQUENCIES` (comma-separated) in one process with one module build, each point from the same data, one CSV row each (else the gate calls it once per point, the percent as its argument); `mounts` are the existing external checkouts/SDKs/caches the verbs need at runtime (the agents' sandbox binds them), `docker` says the verbs need the docker daemon |
| `up` | | start the instance if absent (idempotent), with its empty database; write `$ACCEL_ENGINE_STATE_DIR/env` (`KEY=VALUE` lines the other verbs and harness binaries read, among them the profile's `query_host.dataset_env` holding the instance's database or connection). `build` works after `up` alone |
| `down` | | stop the instance and remove everything it created |
| `load` | `<schema.sql> [constraints.sql]` | create the schema and load `$ACCEL_DATASET_DIR`; idempotent (skip when already loaded from the same dataset); a table whose `.tbl` file is missing or empty stays empty; the loader keeps NULL (bare empty field) and the empty string (`""`) apart |
| `build` | `[target...]` | build the backend and its registered accelerators, then install or load them into the instance. The build dir is configured with `$ACCEL_CMAKE_ARGS` plus what only the backend knows (the `-S`/`-B` paths as its build environment sees them, the engine SDK's locations and the backend's own options), and reconfigured when `$ACCEL_CMAKE_ARGS` changes; nothing but `build` configures it |
| `sql` | `<file>` or `-` | run SQL against the instance; print the result of the last statement as CSV with a header (RFC 4180), NULL as a bare empty cell and the empty string as `""`; warnings and errors go to stderr, never into the CSV |
| `run` | `<target> [args...]` | run a built test or bench binary where it reaches the instance, with the `env` file applied and the caller's `ACCEL_*` variables passed through (gates set thread caps, forced actions and the SQL entry this way) |
| `slot` | `<scenario> <seconds>` | run one slot-gate scenario (`epoch_stress`, `hits_under_writes`, `read_after_commit`, `txn_edges`, `restart`, `own_writes`, `ddl_events`) |

Exit status: `0` pass, `1` fail, `2` usage, `77` not applicable. A verb returns `77` only
when the engine profile states the mechanic is n/a: an embedded engine's `up`/`down`
create and remove its database file and nothing else. `slot` never returns `77`
for a scenario the substrate requires (`own_writes` and `ddl_events` included).

All output goes to stdout/stderr; logs longer than a screen go under
`$ACCEL_ENGINE_STATE_DIR/logs/`.

## Hit contract

A benchmark that quietly measures the fallback SQL looks like a working accelerator, so every backend
proves, through `host.sh` alone, that a read can be told apart from a fallback. The backend ships a
target `test_{engine}_hit_contract` under `tests/` (its own tests never go in `tests/harness/`, which is host_bootstrap's; a name the gate context may override with
`target_paths.hit_contract_test_target`; it is built by `host.sh build` and run by `host.sh run`) that:

1. registers one small fixture accelerator through the backend's registration API (the SQL entry-point
   registrar of `api_contract.md`) on a database whose state is not built;
2. issues the accelerator's first read: it must build the state;
3. issues the second read: it must be a hit, with the runtime's counters showing one more hit, no more
   fallbacks, and no fallback SQL run.

`ACCEL_FALLBACK_TRAP=1` is passed through `host.sh run` (the caller's `ACCEL_*` variables already are):
the backend turns every fallback SQL execution (the runtime's fallback, a transaction-wrote-dependency
replacement) into an error naming the accelerator, and the harness refuses to run without it. Exit status:
`0` pass, `1` the contract is violated, `77` the engine profile states it n/a. The gate is
`engine_hit_contract` (`pipeline/gates/run_engine_hit_contract.sh`).

The same contract is exercised from outside by the pipeline's canary suite (`canary_contract.md`) and, for the benches, by the conformance clauses of `bench_sweep_contract.md`.

Benchmarks report the same facts so `measurement.check_measurement` can tell a hit from a fallback: the
sweep CSV carries `accel_hit_rate` (reads served from the state over all reads), `accel_fallback_reads`
and `state_builds` (counters of the measured run, taken from the runtime's statistics, never assumed).

## API index: `backends/{engine}/api/README.md`

The first table under the heading `## Contract roles` is machine-read. One row per
role named in `api_contract.md`, three columns, backticked entries, comma-separated:

    ## Contract roles

    | role | headers | symbols |
    |---|---|---|
    | registration | `api/register.hpp` | `RegisterQuery`, `QueryRegistrar` |

Every listed header exists; every listed symbol is declared in one of its row's
headers. Prose may follow the table; nothing reads it.
