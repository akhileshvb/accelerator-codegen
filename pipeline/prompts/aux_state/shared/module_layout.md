# Module layout conventions

Use `{{query_number}}` / `q{{query_number}}` placeholders. Derive exact paths in `files_to_create` from analysis; typical roles:

| Role | Typical path |
|------|----------------|
| Public aux API | `accelerators/tpch/q{{query_number}}/<engine>/aux/module.hpp` |
| Aux implementation | `accelerators/tpch/q{{query_number}}/<engine>/aux/module.cpp` |
| Finalize payload | `accelerators/tpch/q{{query_number}}/<engine>/aux/finalize.cpp` |
| Cost model JSON | `accelerators/tpch/q{{query_number}}/config/q{{query_number}}_aux_cost_model.json` |
| Cost-model location | `accelerators/tpch/q{{query_number}}/cost_model.hpp` — `AuxCostModelLocation kTpchQ{{query_number}}CostModel{"accelerators/tpch/q{{query_number}}/config", "q{{query_number}}_aux_cost_model"}` (scaffold writes it); pass it as `SharedQueryCatalogEntry::cost_model` in `duckdb/aux/register.cpp` and to the Postgres / MySQL `Runtime` (peer: `accelerators/tpch/q6/`) |
| Module unit test | none per query: the shared `eval/tests/test_tpch_module.cpp` (ModuleRuntime) is built as `test_tpch_q{{query_number}}_aux` / ctest `tpch_q{{query_number}}_aux_smoke_test` once `duckdb/aux/register.cpp` exists (see `unit_test_contract.md`) |

`aux_correctness` also runs SF1 DuckDB differential via `test_tpch_shared_differential`.

| Write-freq benchmark | `accelerators/tpch/q{{query_number}}/bench/benchmark_q{{query_number}}_aux.cpp` |
| Cost calibration | `accelerators/tpch/q{{query_number}}/bench/calibrate_q{{query_number}}_aux_cost_model.cpp` — **thin wrapper only** (see below) |
| Multi-connection stress | `eval/tests/test_aux_multi_connection_stress.cpp` (gate `aux_multi_connection`) |

### Cost calibrator (required shape)

`calibrate_q{{query_number}}_aux_cost_model.cpp` must **only** delegate to the shared helper — the `aux_cost_model_calibrate` gate rejects custom/stub calibrators:

```cpp
#include "calibrate_aux_cost_model_common.hpp"

int main(int argc, char **argv) {
  return accelerator_codegen::calibration::CalibrateAndWriteCostModel({{query_number}}, argc, argv);
}
```

Do **not** hand-write timing loops or CSV output here; that belongs in `benchmark_q{{query_number}}_aux.cpp`. The gate runs this binary to populate `interference_*` fields in `accelerators/tpch/q{{query_number}}/config/q{{query_number}}_aux_cost_model.json`.

SEC EDGAR: replace `tpch` → `sec_edgar`, cost model → `accelerators/sec_edgar/q{{query_number}}/config/sec_edgar_q{{query_number}}_aux_cost_model.json` and location → `kSecEdgarQ{{query_number}}CostModel{"accelerators/sec_edgar/q{{query_number}}/config", "sec_edgar_q{{query_number}}_aux_cost_model"}`. See `pipeline/prompts/sec_edgar/aux_state_notes.md`.

CMake: the module is discovered from `<engine>/aux/finalize.cpp` (`accel_register_aux_state_queries`, `cmake/AuxState.cmake`); the engine backend discovers bench/calibrate/module-test targets from the query directory. Neither `CMakeLists.txt` nor `cmake/` is edited.

The aux benchmark **must** implement the write-frequency sweep contract in `bench_sweep_contract.md` (not a read-only stub).

**Read path**: `aux/module.cpp` must follow `read_path_scaffold.md` (`CheckValidity`, bounded window reads, `ExecuteFallback`). Port from Q14 (single partition) or Q1/Q10/Q12 (window gates).

Layout reference: `pipeline/prompts/shared/generated_layout.md`.
