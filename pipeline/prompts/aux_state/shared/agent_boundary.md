# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- Partition payload structs and partition keying (`accelerators/{bench}/q{N}/layout.hpp`)
- `InterpretMutation` relevance / column watches
- Incremental accumulators in `CreateTxnBuffer` + `AccumulateMutation`
- `StageMaintenance` action selection inputs and staged partition updates (rebuild must **resync**, not invalidate-only)
- Initial `CostModelParams` coefficients in `accelerators/tpch/q{N}/config/q{N}_aux_cost_model.json`
- `aux/module.{hpp,cpp}`, optional `aux/finalize.{hpp,cpp}`
- Query-specific tests, benchmarks, and calibration binaries
- **Shared registration / helpers (append-only):** every catalog, allowlist, host SQL entrypoint for the workload `engine_name` (default duckdb), cross-accel / multi-conn helper, and bench-arg surface that neighboring aux queries already use — discover paths by searching a peer query id; do not invent a partial wiring or a second adapter tree

## Fixed substrate (do not regenerate from query pipelines)

Under `substrate/aux/`:

- **`core/`** — `AuxQueryModule`, `AuxEngineServices`, `AuxStateStore`, `AuxRegistry`, `aux_calibration.hpp`, `byte_stream.hpp`
- **`state/`** — `PartitionedStateStore`, invalidation plans, partition keys
- **`maintenance/`** — `MaintenanceAction`, `ActionCostEstimate`, `CostModelParams`, `SelectMinCostAction`

## Shared primitives library (`accelerators/prims/`) — growable, but only through `primitive_publish`

`accelerators/prims` holds the generic, workload-agnostic data structures and value helpers accelerator state is built from (copy-on-write arrays/maps/record stores/blob maps, sorted/Fenwick/radix structures, `sql_date`, `sql_like`, exact decimal/int128). Catalogue and "Which one to use": `prims/README.md`.

- **Search it first.** Build the state from existing primitives. Read the README, then the header comment (PURPOSE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN) of each candidate.
- **Never edit `prims/`, `prims/README.md` or `accelerators/prims/tests/test_prims.cpp` directly** (writes are denied; they are shared by every concurrent worker). To add a primitive: stage the new header, the full updated README and the full updated `test_prims.cpp` under `pipeline/prims_staging/<same repo-relative path>` and call the **`primitive_publish(files, rationale)`** tool. It checks additions-only, header comment, README row, tests, and no workload vocabulary; builds and runs `test_prims`; and only then makes it visible to everyone. After a successful publish your `prims/` copy is refreshed, so just `#include` it.
- **Additions only.** New headers, or new members/lines in an existing header. Never change or remove an existing line, signature or behavior; prefer a new primitive over modifying an existing one.
- **Generic only.** Name a primitive by what it is (`ShardedSortedMap`), never by who uses it. No table/column names, benchmark names or query numbers anywhere in it, comments included.
- Workload-specific structures (an inverted index for one LIKE predicate, a query-specific cube layout) stay in your module, but must still be copy-on-write and built from primitives.

## Engine backends (`backends/{engine}/`)

Owned by the **engine_backend** pipeline (`workload_kind: engine_backend`), not per-query agents.

- Query agents must **not** invent new engine adapters mid-query.
- Existing adapters (when present) are optional few-shot only; they are regenerable from the substrate via the engine pipeline.
- Headers marked `GENERATED BACKEND` means engine-pipeline-owned.

Shared generated glue is **append-only** from query agents (register this query everywhere peers are registered; search to find current files). Prefer minimal appends over rewrites. CMake auto-discovery usually covers test/bench/calibrate; touch CMake only when targets are missing.

## Store-mode notes (examples)

- **typed_in_process**: typed `StateImage` payloads; no serialize on hot path
- **serialized_boundary**: encode/decode only at a process/SHM boundary (`byte_stream.hpp`); same `AuxQueryModule` logic

See `pipeline/prompts/shared/engine_interfaces.md`, `generated_layout.md`, and `hard_guidelines.md`.

## Registration checklist

See `registration_checklist.md` in this directory — **register everywhere**, then verify with `run_correctness` + `run_multi_connection`.

## Runtime policy

Default: hybrid cost-based selector via `AuxEngineServices::ChooseMaintenance`.

## Concurrency

See `concurrency_contract.md`. Irrelevant mutations → `NoOp`; bounded read paths; correct `ExecuteFallback`; rebuild → resync.
