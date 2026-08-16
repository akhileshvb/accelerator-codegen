# Module layout conventions

Use `{{query_number}}` / `q{{query_number}}` placeholders. Derive exact paths in `files_to_create` from analysis; typical roles:

| Role | Typical path |
|------|----------------|
| Public aux API | `src/generated/tpch/q{{query_number}}/aux/module.hpp` |
| Aux implementation | `src/generated/tpch/q{{query_number}}/aux/module.cpp` |
| Finalize payload | `src/generated/tpch/q{{query_number}}/aux/finalize.cpp` |
| Cost model JSON | `config/q{{query_number}}_aux_cost_model.json` |
| Tests | `tests/single-threaded/test_tpch_q{{query_number}}_aux.cpp`, `test_q{{query_number}}_*.cpp` |

Unit smoke fixtures must follow `unit_test_contract.md` (SQL polarity). `aux_correctness` also runs SF1 DuckDB differential via `test_tpch_shared_differential`.

| Write-freq benchmark | `benchmarks/tpch/single-threaded/benchmark_q{{query_number}}_aux.cpp` |
| Cost calibration | `benchmarks/tpch/single-threaded/calibrate_q{{query_number}}_aux_cost_model.cpp` — **thin wrapper only** (see below) |
| Multi-connection stress | `tests/aux_state/test_aux_multi_connection_stress.cpp` (gate `aux_multi_connection`) |

### Cost calibrator (required shape)

`calibrate_q{{query_number}}_aux_cost_model.cpp` must **only** delegate to the shared helper — the `aux_cost_model_calibrate` gate rejects custom/stub calibrators:

```cpp
#include "calibrate_aux_cost_model_common.hpp"

int main(int argc, char **argv) {
  return accelerator_codegen::calibration::CalibrateAndWriteCostModel({{query_number}}, argc, argv);
}
```

Do **not** hand-write timing loops or CSV output here; that belongs in `benchmark_q{{query_number}}_aux.cpp`. The gate runs this binary to populate `interference_*` fields in `config/q{{query_number}}_aux_cost_model.json`.

SEC EDGAR: replace `tpch` → `sec_edgar` and cost model → `config/sec_edgar_q{{query_number}}_aux_cost_model.json`. See `pipeline/prompts/sec_edgar/aux_state_notes.md`.

CMake: test/bench/calibrate targets are **auto-discovered** from `test_tpch_q*_aux.cpp` via `accel_register_tpch_aux_pipeline_targets()` in `CMakeLists.txt`. Edit `CMakeLists.txt` only when auto-discovery is insufficient; follow patterns in `cmake/AuxState.cmake` and neighboring queries.

The aux benchmark **must** implement the write-frequency sweep contract in `bench_sweep_contract.md` (not a read-only stub).

**Read path**: `aux/module.cpp` must follow `read_path_scaffold.md` (`CheckValidity`, bounded window reads, `ExecuteFallback`). Port from Q14 (single partition) or Q1/Q10/Q12 (window gates).

Layout reference: `pipeline/prompts/shared/generated_layout.md`.
