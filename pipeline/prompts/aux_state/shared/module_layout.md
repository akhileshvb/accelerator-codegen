# Module layout conventions

Use `{{query_number}}` / `q{{query_number}}` placeholders. Derive exact paths in `files_to_create` from analysis; typical roles:

| Role | Typical path |
|------|----------------|
| Public aux API | `include/accelerator_codegen/tpch_q{{query_number}}_aux.hpp` |
| Incremental | `include/.../tpch_q{{query_number}}_incremental.hpp`, `src/aux_state/q{{query_number}}/q{{query_number}}_incremental.cpp` |
| Maintenance | `include/.../tpch_q{{query_number}}_maintenance.hpp`, `..._maintenance_policy.hpp`, `src/.../q{{query_number}}_maintenance_policy.cpp` |
| Mutation features | `src/aux_state/q{{query_number}}/q{{query_number}}_mutation_features.cpp` |
| Workload features | `src/aux_state/q{{query_number}}/q{{query_number}}_workload_features.cpp` |
| Cost formulas | `src/aux_state/q{{query_number}}/q{{query_number}}_cost_formulas.cpp` |
| Rebuild / write policy | `src/aux_state/q{{query_number}}/q{{query_number}}_rebuild_policy.cpp` |
| Dependencies / registration | `src/aux_state/q{{query_number}}/q{{query_number}}_aux_dependencies.cpp` |
| Kernel / package | `src/aux_state/q{{query_number}}/q{{query_number}}_aux_kernel.cpp`, `q{{query_number}}_aux_package.cpp` |
| Cost model JSON | `config/q{{query_number}}_aux_cost_model.json` |
| Tests | `tests/single-threaded/test_tpch_q{{query_number}}_aux.cpp`, `test_q{{query_number}}_*.cpp` |
| Write-freq benchmark | `benchmarks/tpch/single-threaded/benchmark_q{{query_number}}_aux.cpp` |
| Cost calibration | `benchmarks/tpch/single-threaded/calibrate_q{{query_number}}_aux_cost_model.cpp` (includes `interference_*` via multi-connection calibration) |
| Multi-connection stress | `tests/aux_state/test_aux_multi_connection_stress.cpp` (gate `aux_multi_connection`) |

CMake: `accel_add_aux_state_query(QUERY_NUM {{query_number}} ...)` and `accel_add_aux_state_benchmarks` per `cmake/AuxState.cmake`.

The aux benchmark **must** implement the write-frequency sweep contract in `bench_sweep_contract.md` (not a read-only stub).

**Concurrency**: `*_aux_package.cpp` must follow `read_path_scaffold.md` (reader epoch pin, execution gate, `MaxThreads() = 1`, bounded window iteration). Port from Q14 (single partition) or Q1/Q10/Q12 (window gates).
