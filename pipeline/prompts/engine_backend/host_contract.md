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
| `ACCEL_DATASET_DIR` | engine-neutral dataset: one `<table>.tbl` (`|`-separated, TPC-H dbgen format) per table |
| `ACCEL_CMAKE_ARGS` | the arguments the build dir is configured with (generator, build type, this backend's switch, the engine's query sources) |

## Verbs

| verb | args | does |
|---|---|---|
| `info` | | print one JSON line: `{"engine", "embedded": bool, "verbs": [...], "hit_contract": true, "mounts": {"ro": [abs paths], "rw": [abs paths]}, "docker": bool}`; `hit_contract` is `true` once the backend meets the hit contract below (a backend without it is skipped by that gate); `mounts` are the existing external checkouts/SDKs/caches the verbs need at runtime (the agents' sandbox binds them), `docker` says the verbs need the docker daemon |
| `up` | | start the instance if absent (idempotent), with its empty database; write `$ACCEL_ENGINE_STATE_DIR/env` (`KEY=VALUE` lines the other verbs and harness binaries read). `build` works after `up` alone |
| `down` | | stop the instance and remove everything it created |
| `load` | `<schema.sql> [constraints.sql]` | create the schema and load `$ACCEL_DATASET_DIR`; idempotent (skip when already loaded from the same dataset) |
| `build` | `[target...]` | build the backend and its registered accelerators, then install or load them into the instance. The build dir is configured with `$ACCEL_CMAKE_ARGS` plus what only the backend knows (the `-S`/`-B` paths as its build environment sees them, the engine SDK's locations and the backend's own options), and reconfigured when `$ACCEL_CMAKE_ARGS` changes; nothing but `build` configures it |
| `sql` | `<file>` or `-` | run SQL against the instance; print the result of the last statement as CSV with a header |
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
harness target `test_{engine}_hit_contract` (a name the gate context may override with
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
