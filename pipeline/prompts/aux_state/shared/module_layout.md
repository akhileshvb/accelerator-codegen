# Module layout conventions

Use `{{query_number}}` / `q{{query_number}}` placeholders and the run's `{bench}`. Derive exact paths in `files_to_create` from analysis; typical roles:

| Role | Typical path |
|------|----------------|
| Public aux API | `accelerators/{bench}/q{{query_number}}/<engine>/aux/module.hpp` |
| Aux implementation | `accelerators/{bench}/q{{query_number}}/<engine>/aux/module.cpp` |
| Finalize payload | `accelerators/{bench}/q{{query_number}}/<engine>/aux/finalize.cpp` |
| Cost model JSON | `accelerators/{bench}/q{{query_number}}/config/` (file stem as the scaffolded `cost_model.hpp` names it) |
| Cost-model location | `accelerators/{bench}/q{{query_number}}/cost_model.hpp` (scaffold writes it); pass it to the registration (see the **registration** role of the Backend API) |
| Module unit test | none per query: the shared module test is built per query once the query's registration file exists (see `unit_test_contract.md`) |

`aux_correctness` also runs the SF1 differential against the engine's own results.

| Write-freq benchmark | `accelerators/{bench}/q{{query_number}}/bench/benchmark_q{{query_number}}_aux.cpp` |
| Cost calibration | `accelerators/{bench}/q{{query_number}}/bench/calibrate_q{{query_number}}_aux_cost_model.cpp` — **thin wrapper only** (see below) |
| Multi-connection stress | the shared stress harness (gate `aux_multi_connection`) |

### Cost calibrator (required shape)

`calibrate_q{{query_number}}_aux_cost_model.cpp` must **only** delegate to the shared calibration helper, in the shape the scaffolded stub or a peer's calibrator has — the `aux_cost_model_calibrate` gate rejects custom/stub calibrators.

Do **not** hand-write timing loops or CSV output here; that belongs in `benchmark_q{{query_number}}_aux.cpp`. The gate runs this binary to populate `interference_*` fields in the query's cost-model JSON.

See `pipeline/prompts/sec_edgar/aux_state_notes.md` for benchmark-specific naming.

CMake: bench/calibrate targets are **auto-discovered**, and the module test target is created from the query's registration file. Edit `CMakeLists.txt` only when auto-discovery is insufficient; follow `cmake/AuxState.cmake` and neighboring queries.

The aux benchmark **must** implement the write-frequency sweep contract in `bench_sweep_contract.md` (not a read-only stub).

**Read path**: `aux/module.cpp` must follow `read_path_scaffold.md` (`CheckValidity`, bounded window reads, `ExecuteFallback`). Port from a peer (single partition, or window gates).

Layout reference: `pipeline/prompts/shared/generated_layout.md`.
