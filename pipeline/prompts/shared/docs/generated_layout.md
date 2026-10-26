# Generated code layout contract

| Layer | Path | Writer |
|---|---|---|
| Substrate | `substrate/**` (`aux/{core,state,maintenance,runtime}`, `verus/{common,store,kit,sql2verus}`) | nobody |
| Engine backend | `backends/{engine}/**` (`api/` incl. `api/catalog/`, `engine/`, `state/`, `tests/`, `bench/`) | that engine's `engine_backend` run only |
| Query | `accelerators/{bench}/q{N}/**` | that query's run only |
| Primitives | `accelerators/prims/**` | only `primitive_publish` |
| Proofs | `accelerators/prooflib/**` | only `prooflib_publish` |
| Workloads | `workloads/**` (query SQL, schemas) | nobody |

Include roots are the repo root and `accelerators/`; headers sit next to their sources (no `include/` tree):

```cpp
#include "{bench}/q{N}/{engine}/base/query.hpp"     // accelerators/ root
#include "{bench}/q{N}/{engine}/aux/module.hpp"
#include "prims/<header>.hpp"                       // only what the library lists
#include "substrate/aux/core/aux_types.hpp"         // repo root
#include "backends/{engine}/api/<header>.hpp"       // by role, from the backend API index
```

There is no shared portable `DataChunk` IR: base kernels are engine-specific; only the aux/txn contracts are shared.

## Backend tree (by runtime slot)

A backend implements the slots of `substrate/aux/runtime/engine_slots.hpp` only (`engine_interfaces.md`, "Backend contract"). The runtime flow lives in `substrate/aux/runtime/`, never here.

```
backends/{engine}/
  engine/capture*   # S1: engine change -> ColumnarBatch -> RuntimeHost::OnCapture
  engine/scan*      # S5 build snapshot + S8 partitioned native scans
  state/            # S2-S4 where the engine needs shared state (profile process_model says when)
  api/              # S6 placement + host (EngineSlots implementation), S7 SQL entry points, registries, README.md (role index)
  tests/            # test_{engine}_<layer>.cpp, one per implementation layer
  bench/
```

## Per-query tree

```
accelerators/<bench>/q{N}/
  layout.hpp          # state layout (slot ids), shared by the query's engines
  cost_model.hpp      # AuxCostModelLocation (config directory and file stem); scaffold writes it
  {engine}/
    base/
      query.hpp       # bind/kernel API (symbol prefix <bench>_qN); the header is always query.hpp
      fused.hpp       # optional fused-scan API (host-dependent)
      single-threaded/  # kernel.cpp, package.cpp, fused_scan.cpp, register.cpp
      parallel/         # optional kernel.cpp, package.cpp, register.cpp
    aux/
      module.hpp, module.cpp
      finalize.hpp, finalize.cpp   # optional payload helpers
      register.cpp                 # the query's registrars, where the engine has per-query registration files
  tests/              # test_{bench}_q{N}.cpp, q{N}_manifest.hpp (the aux module test is shared: unit_test_contract.md)
  bench/              # benchmark_q{N}.cpp, benchmark_q{N}_aux.cpp, calibrate_q{N}_aux_cost_model.cpp
  config/             # q{N}_aux_cost_model[_<engine>][_sf<N>].json (sec_edgar: sec_edgar_q{N}_aux_cost_model.json; see pipeline/config/benchmark_packs/sec_edgar_aux_state_notes.md)
    verus/            # Verus variants (ACCEL_AUX_COST_MODEL_DIR=verus selects them)
  verus_kit/<crate>/  # Verus accelerator: glue, C header, kernel crate
```

Benchmark-level helpers live in `accelerators/{bench}/common/`; cross-query harnesses (differential drivers, cross-accel helpers, arg variants) in `eval/`.

**Hard rule:** write only under `accelerators/{bench}/q{N}/{engine}/` (plus the query's own `tests/`, `bench/`, `config/`) for the run's `engine_name`. `engine_name` is required; never create or reintroduce a flat `q{N}/base/` or `q{N}/aux/` tree. Whether base tables stay in engine-native storage and where aux state may live is in the engine profile. Prefer peers under the same `{engine}/` tree as references (read-only).

Rules for the files above:

- Fused scan is hooked up through the package where the backend API offers a fused-scan hook.
- `bench/benchmark_q{N}_aux.cpp` implements the write-frequency sweep of `bench_sweep_contract.md`.
- `bench/calibrate_q{N}_aux_cost_model.cpp` only delegates to the shared calibration helper, in the shape of the scaffolded stub or a peer's (the `aux_cost_model_calibrate` gate rejects custom calibrators; timing loops and CSV output belong in the aux benchmark).
- The multi-connection stress harness is shared (gate `aux_multi_connection`); the cost-model JSON in `config/` is named by `cost_model.hpp` and passed to the registration.

`aux_correctness` also runs the iteration-scale differential against the engine's results. `aux/module.cpp` follows `read_path_scaffold.md`.

CMake: neither the root `CMakeLists.txt` nor `cmake/` is edited. Each `{engine}/aux/finalize.cpp` is discovered (`accel_register_aux_state_queries`, `cmake/AuxState.cmake`); bench/calibrate targets are the backend's shared harness; a benchmark's targets live in `accelerators/{bench}/build.cmake`, an engine's in `backends/{engine}/backend.cmake` (globs per `cmake/Backend.cmake`).

## Registration: the query registers itself (backends name no query)

Every backend exposes registries; a query plugs in with static registrars from its own directory. Adding a query never edits a backend file, catalog array, dispatch switch or function list; a tree with every `accelerators/<bench>/q<N>/` removed still builds the backends.

The registrars, registries and entry-point shims are the **registration** role of the backend API index (`backends/{engine}/api/README.md`, rendered in your prompt as "Backend API"): the engine's registrars for base kernels, catalog entries and SQL entry points. Put the query's registration files under `q{N}/{engine}/` (peers show the split) and use only the index's registrars. A Verus accelerator registers through `substrate/verus/kit/verus_kit.hpp` (`kit::Registrar`) from `q{N}/verus_kit/<crate>/`.

Every query ships `cost_model.hpp`: pass it to the registration (the registrar's `cost_model` field or argument); the substrate picks the engine and scale-factor variant. Never hard-code a JSON path.

**Backend additions.** Backend files belong to the engine_backend run; the write is denied. When a query needs an engine-level helper the backend lacks, log a request and do not rely on the helper in this run: Verus pipeline, the `backend_addition_record` tool; C++ pipeline, one JSON line `{"benchmark", "query", "scope", "helper", "reason"}` appended to `pipeline/artifacts/backend_additions/{engine}.jsonl` (`scope` is `"backend"` or `"harness"`; no query names in `helper`; see Ownership in `query_host_bootstrap.md`). The helper must be generic; never put per-query code in the backend.

Related: `engine_interfaces.md`; process-boundary encoding only for a shipped-worker placement: `substrate/aux/runtime/wire.hpp`.
