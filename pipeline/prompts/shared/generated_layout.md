# Generated code layout contract

The repository has one directory per layer, and each layer has one write rule:

| Layer | Path | Who writes it |
|---|---|---|
| Substrate | `substrate/**` (`aux/{core,state,maintenance,runtime}`, `verus/{common,store,kit,sql2verus}`) | **nobody** (read-only to every agent) |
| Engine backend | `backends/{engine}/**` (`api/` incl. `api/catalog/`, `engine/`, `state/`, `tests/`, `bench/`; organized by slot, see below) | that engine's `workload_kind: engine_backend` run only |
| Query | `accelerators/{bench}/q{N}/**` | that query's run only |
| Primitives library | `accelerators/prims/**` | only through `primitive_publish` |
| Proof library | `accelerators/prooflib/**` | only through `prooflib_publish` |
| Workloads | `workloads/**` (query SQL, schemas) | **nobody** (read-only) |

Agent-produced query code lives under `accelerators/{bench}/q{N}/`, **scoped by `engine_name`**.
The include roots are the repo root and `accelerators/`; use short includes:

```cpp
#include "{bench}/q{N}/{engine}/base/query.hpp"     // accelerators/ root
#include "{bench}/q{N}/{engine}/aux/module.hpp"
#include "prims/<header>.hpp"                       // accelerators/prims/, only what the library lists
#include "substrate/aux/core/aux_types.hpp"         // repo root
#include "backends/{engine}/api/<header>.hpp"       // by role, from the backend API index
```

Headers sit next to their sources (there is no `include/` tree).

There is **no** shared portable `DataChunk` IR. Base kernels are engine-specific; only aux/txn contracts are shared.

## Backend tree (by runtime slot)

A backend implements the slots of `substrate/aux/runtime/engine_slots.hpp` and nothing else (see `engine_interfaces.md`, "Backend contract"):

```
backends/{engine}/
  engine/capture*       # S1: engine change -> ColumnarBatch -> RuntimeHost::OnCapture
  engine/scan*          # S5 build snapshot + S8 partitioned native scans
  state/                # S2-S4 where the engine needs shared state (the engine profile's process_model says when)
  api/                  # S6 placement + the host (the EngineSlots implementation), S7 SQL entry points, registries, README.md (the role index)
  tests/                # test_{engine}_<layer>.cpp, one per implementation layer (capture, epochs, scan, runtime, host)
  bench/
```

The runtime flow (ordering, catch-up, builds, version management, thread pools) is `substrate/aux/runtime/`; never put it here.

## Per-query tree

Everything a query run owns is under one directory, so one glob covers it:

```
accelerators/{tpch|sec_edgar}/q{N}/
  layout.hpp                 # the query's state layout (slot ids), shared by its engines
  cost_model.hpp             # the query's AuxCostModelLocation (config directory and file stem)
  {engine}/
    base/
      query.hpp              # bind/kernel API (symbol prefix still tpch_qN / sec_edgar_qN)
      fused.hpp              # optional fused-scan API (host-dependent)
      single-threaded/       # kernel.cpp, package.cpp, fused_scan.cpp
      single-threaded/register.cpp   # where the engine registers base kernels per query
      parallel/              # optional parallel kernel.cpp, package.cpp, register.cpp
    aux/
      module.hpp, module.cpp
      finalize.hpp, finalize.cpp     # optional payload helpers
      register.cpp           # the query's registrars, where the engine has per-query registration files
  tests/                     # test_{bench}_q{N}.cpp, q{N}_manifest.hpp, ... (the aux module test is shared, see unit_test_contract.md)
  bench/                     # benchmark_q{N}.cpp, benchmark_q{N}_aux.cpp, calibrate_q{N}_aux_cost_model.cpp
  config/                    # q{N}_aux_cost_model[_<engine>][_sf<N>].json (sec_edgar: sec_edgar_q{N}_aux_cost_model.json)
    verus/                   # the Verus accelerator's variants (ACCEL_AUX_COST_MODEL_DIR=verus selects them)
  verus_kit/<crate>/         # Verus accelerator: glue, C header, kernel crate
```

Benchmark-level helpers shared by a benchmark's queries live in `accelerators/{bench}/common/`
(shared by that benchmark's queries); cross-query evaluation
harnesses (differential drivers, cross-accel helpers, arg variants) live in `eval/`.

## Registration: the query registers itself (backends name no query)

Every engine backend exposes **registries**; a query plugs in with static registrars
from **its own directory**. Adding a query never edits a backend file, a catalog
array, a dispatch switch, or a function list; a tree with every
`accelerators/<bench>/q<N>/` removed still builds the backends.

The registrars, registries and entry-point shims are the **registration** role of the
backend API index (`backends/{engine}/api/README.md`; rendered in your prompt as
"Backend API"). The neutral surface (`RegisterQuery` / `QueryRegistrar`, `QuerySpec`)
works on every engine; the index lists the engine's own registrars for base kernels,
catalog entries and SQL entry points. Place the query's registration files under
`q{N}/{engine}/` (peers show the file split) and use only the index's registrars.
A Verus accelerator registers through `substrate/verus/kit/verus_kit.hpp`
(`kit::Registrar`) from `q{N}/verus_kit/<crate>/`.

Every query ships `accelerators/{bench}/q{N}/cost_model.hpp` (scaffold generates it):
one `AuxCostModelLocation` naming the query's `config/` directory and file stem. Pass
it to the registration (the registrar's `cost_model` field or argument); the substrate
picks the engine and scale-factor variant from it. Never hard-code a JSON path.

Backend files (`backends/{engine}/**`) belong to that engine's `engine_backend` run.
A query run never writes them (the write is denied): when a query needs an
**engine-level** helper the backend lacks, log a request for the engine_backend
run (Verus pipeline: the `backend_addition_record` tool; C++ pipeline: one JSON line
`{"benchmark", "query", "helper", "reason"}` appended to
`pipeline/artifacts/backend_additions/{engine}.jsonl`). The helper must be generic
(no query names); never put per-query code in the backend, and do not rely on the
helper in the current run.

Multiple engines may coexist under the same `q{N}/` directory (one subdirectory per engine).

**Hard rule:** write only under `accelerators/{bench}/q{N}/{engine}/` (plus the query's own
`tests/`, `bench/`, `config/`) for the run's `engine_name`. Do **not** create or modify flat
`q{N}/base/` or `q{N}/aux/` trees. Whether base tables stay in engine-native storage and where
aux state may live is in the engine profile.

## Base kernel files (agents generate)

| Role | Path |
|------|------|
| Public API | `accelerators/{benchmark}/q{N}/{engine}/base/query.hpp` |
| Kernel | `accelerators/{benchmark}/q{N}/{engine}/base/single-threaded/kernel.cpp` |
| Host package | `accelerators/{benchmark}/q{N}/{engine}/base/single-threaded/package.cpp` |
| Fused scan (optional) | `.../fused_scan.cpp` + `base/fused.hpp`; hooked up through the package, where the backend API offers a fused-scan hook |
| Registration | the query's registrar file(s), table above |
| Tests | `accelerators/{benchmark}/q{N}/tests/test_{benchmark}_q{N}.cpp` (or engine-qualified peers) |
| Benchmark | `accelerators/{benchmark}/q{N}/bench/benchmark_q{N}.cpp` |

**`engine_name` is required** — never assume an engine or invent a flat `q{N}/base/` path without the engine segment.

**Do not edit** a backend's registration code or registries: register through the index's registrars from the query's own directory — do not invent a parallel adapter tree.

`CMakeLists.txt` is editable for CMake target registration when auto-discovery is insufficient; follow `cmake/AuxState.cmake` and the globs in `backends/{engine}/backend.cmake` and existing query blocks.

## Aux-state files (agents generate)

| Role | Path |
|------|------|
| Module API | `accelerators/{benchmark}/q{N}/{engine}/aux/module.hpp` |
| Module impl | `accelerators/{benchmark}/q{N}/{engine}/aux/module.cpp` |
| Finalize helpers | `accelerators/{benchmark}/q{N}/{engine}/aux/finalize.{hpp,cpp}` |
| Cost model | `accelerators/{benchmark}/q{N}/config/` JSON named by `cost_model.hpp` |
| Cost-model location | `accelerators/{benchmark}/q{N}/cost_model.hpp` (passed to every engine's registration; see above) |
| Tests / bench | the shared module test (no per-query file); `accelerators/{benchmark}/q{N}/bench/benchmark_q{N}_aux.cpp` |

CMake: `accel_add_aux_state_query(BENCHMARK {benchmark} QUERY_NUM N ...)` in `cmake/AuxState.cmake` (engine-aware discovery).

## Naming vs paths

Workload YAML uses symbol prefixes like `<bench>_q<N>`. Those are **API names**, not header filenames. The header file is always `base/query.hpp` under the `{engine}/` segment.

## Reference implementations

Prefer peers under the **same** `{engine}/` tree when present (read-only: other queries' directories are never writable). Flat `q{N}/base/` paths are legacy and must not be reintroduced.

| Pattern | Read first |
|---------|------------|
| Base kernel | `accelerators/tpch/q*/{engine}/base/single-threaded/kernel.cpp` |
| Fused scan | a peer's `fused_scan.cpp`, `base/fused.hpp` and `package.cpp` when present for that host |
| Registration | a peer's registrar file(s) (table above) |
| Aux module | `accelerators/tpch/q*/{engine}/aux/module.cpp` |
| Process-boundary encoding | `substrate/aux/runtime/wire.hpp` (only for a shipped-worker placement) |

See also `pipeline/prompts/shared/engine_interfaces.md`.
