# Engine host contract

Engine-agnostic. Every backend provides one executable, `backends/{engine}/host.sh`,
and kept code (gates, orchestrator, scripts, eval drivers) reaches the engine only
through it. Nothing outside `backends/{engine}/` names the engine's server, client,
container, plugin or data layout.

    backends/{engine}/host.sh <verb> [args...]

## Environment (set by the caller)

| variable | meaning |
|---|---|
| `ACCEL_ENGINE_INSTANCE` | instance name, unique per run; containers, sockets, ports and DB names derive from it, so parallel runs never collide |
| `ACCEL_ENGINE_STATE_DIR` | directory the instance owns (data, logs, `env`); delete it and the instance is gone |
| `ACCEL_BUILD_DIR` | the CMake build directory |
| `ACCEL_DATASET_DIR` | engine-neutral dataset: one `<table>.tbl` (`|`-separated, TPC-H dbgen format) per table |

## Verbs

| verb | args | does |
|---|---|---|
| `info` | | print one JSON object: `{"engine", "embedded": bool, "verbs": [...]}` |
| `up` | | start the instance if absent (idempotent); write `$ACCEL_ENGINE_STATE_DIR/env` (`KEY=VALUE` lines the other verbs and harness binaries read) |
| `down` | | stop the instance and remove everything it created |
| `load` | `<schema.sql> [constraints.sql]` | create the schema and load `$ACCEL_DATASET_DIR`; idempotent (skip when already loaded from the same dataset) |
| `build` | `[target...]` | build the backend and its registered accelerators, then install or load them into the instance |
| `sql` | `<file>` or `-` | run SQL against the instance; print the result of the last statement as CSV with a header |
| `run` | `<target> [args...]` | run a built test or bench binary where it reaches the instance, with the `env` file applied |
| `slot` | `<scenario> <seconds>` | run one slot-gate scenario (`epoch_stress`, `hits_under_writes`, `read_after_commit`, `txn_edges`, `restart`, `own_writes`, `ddl_events`, `differential`) |

Exit status: `0` pass, `1` fail, `2` usage, `77` not applicable. A verb returns `77` only
when the engine profile states the mechanic is n/a: an embedded engine's `up`/`down`
create and remove its database file and nothing else. `slot` never returns `77`
for a scenario the substrate requires.

All output goes to stdout/stderr; logs longer than a screen go under
`$ACCEL_ENGINE_STATE_DIR/logs/`.

## API index: `backends/{engine}/api/README.md`

The first table under the heading `## Contract roles` is machine-read. One row per
role named in `api_contract.md`, three columns, backticked entries, comma-separated:

    ## Contract roles

    | role | headers | symbols |
    |---|---|---|
    | registration | `api/register.hpp` | `RegisterQuery`, `QueryRegistrar` |

Every listed header exists; every listed symbol is declared in one of its row's
headers. Prose may follow the table; nothing reads it.
