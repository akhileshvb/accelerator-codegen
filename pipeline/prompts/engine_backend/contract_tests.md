# Engine ↔ substrate layer tests (implementation-owned)

Every `engine_backend` implementation ships **one harness per layer** (see
`pipeline/prompts/engine_backend/layers.md`). Tests prove the adapter against
the fixed substrate **without** generated query accelerators
(`src/generated/{tpch|sec_edgar}/q*/**`).

Soft stubs, “throws when build flag OFF”, `(void)` placeholder asserts,
**SKIP-then-PASS**, or **client-protocol scan shims** are **failures**.

Semantic acceptance is the closed set in `acceptance_checklist.md`, mapped onto
layers. Architecture audit judges the **same** rules.

## Deliverables (per layer)

| Layer | Conventional path | Gate |
|-------|-------------------|------|
| `store` | `tests/engine_backend/test_{engine}_store.cpp` | `engine_store` |
| `aux_backend` | `tests/engine_backend/test_{engine}_aux_backend.cpp` | `engine_aux_backend` |
| `txn` | `tests/engine_backend/test_{engine}_txn.cpp` | `engine_txn` |
| `mutations` | `tests/engine_backend/test_{engine}_mutations.cpp` | `engine_mutations` |
| `scan` | `tests/engine_backend/test_{engine}_scan.cpp` | `engine_scan` |
| `runtime` | `tests/engine_backend/test_{engine}_runtime.cpp` | `engine_runtime` |
| `visibility` | `tests/engine_backend/test_{engine}_visibility.cpp` | `engine_visibility` |
| `host` | `tests/engine_backend/test_{engine}_host.cpp` | `engine_host` (skip if host not required) |

Start from `pipeline/templates/engine_backend/test_engine_layer.cpp.tpl`.
Register via `cmake_layer_test.cmake.tpl`.

Validation authors unseen `test_{engine}_adversarial.cpp` (`adversarial_tests.md`).

## Hard rules (fail closed)

1. **Exercise real native paths** for that layer (in-process storage / txn / hook APIs).
   **Forbidden** for scan: client-protocol round-trips that issue SQL text over a
   network/driver API and parse result rows.
2. **Assert values**, not merely “didn’t crash”.
3. **No TODO / `Fail("…not implemented")` left in `main`.**
4. **No TPC-H / SEC modules** or other generated accelerators.
5. **Seed data inside the harness** (or a tiny fixture owned by `tests/engine_backend/`).
6. **No soft skip.** If the engine SDK / host cannot start, exit non-zero.
7. **Layer `mutations`:** drive INSERT/UPDATE/DELETE through the real capture hook.
   Hand-built `MutationBatch` is allowed in `aux_backend` / `txn` only.
8. **Layer `host`:** if `requires_in_process_host`, ship real in-process registration
   and call the toy Runtime through SQL/UDF.

Host connection details come from the workload `host_runtime` / `engine_sdk_hints`
block and `discover_host_sdk` — do not assume a particular engine’s layout.
