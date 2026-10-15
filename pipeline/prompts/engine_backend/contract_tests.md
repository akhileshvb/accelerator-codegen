# Backend <-> runtime layer tests (implementation-owned)

Every `engine_backend` implementation ships **one harness per layer** (see
`layers.md`). Tests prove the backend's slots against the fixed runtime
skeleton **without** generated query accelerators
(`accelerators/{tpch|sec_edgar}/q*/**`): a toy module on a real `RuntimeHost`
with this backend's `EngineSlots`.

Soft stubs, "throws when build flag OFF", `(void)` placeholder asserts,
**SKIP-then-PASS**, or **client-protocol scan shims** are **failures**.

Semantic acceptance is the closed set in `acceptance_checklist.md`, mapped onto
layers. Architecture audit judges the **same** rules.

## Deliverables (per layer)

| Layer | Conventional path | Gate |
|-------|-------------------|------|
| `capture` | `backends/{engine}/tests/test_{engine}_capture.cpp` | `engine_capture` |
| `epochs` | `backends/{engine}/tests/test_{engine}_epochs.cpp` | `engine_epochs` |
| `scan` | `backends/{engine}/tests/test_{engine}_scan.cpp` | `engine_scan` |
| `runtime` | `backends/{engine}/tests/test_{engine}_runtime.cpp` | `engine_runtime` |
| `host` | `backends/{engine}/tests/test_{engine}_host.cpp` | `engine_host` (skip if host not required) |

Start from `pipeline/templates/engine_backend/test_engine_layer.cpp.tpl`.
Register via `cmake_layer_test.cmake.tpl`. The toy module: copy `SumModule`
and its sink from `eval/tests/test_aux_runtime.cpp`.

Validation authors unseen `test_{engine}_adversarial.cpp`
(`adversarial_tests.md`) and the orchestrator runs the backend-generic slot
gates (`pipeline/gates/README_runtime_gates.md`).

## Hard rules (fail closed)

1. **Exercise real native paths** for that layer (the engine's capture hook,
   commit lifecycle, snapshot and scan APIs). **Forbidden** for scan:
   client-protocol round-trips that issue SQL text over a driver API and parse
   result rows.
2. **Assert values**, not merely "didn't crash". Concurrency asserts use sync
   barriers, never bare `sleep`.
3. **No TODO / `Fail("...not implemented")` left in `main`.**
4. **No TPC-H / SEC modules** or other generated accelerators.
5. **Seed data inside the harness** (or a tiny fixture owned by `backends/{engine}/tests/`).
6. **No soft skip.** If the engine SDK / host cannot start, exit non-zero.
7. **Layer `capture`:** drive INSERT/UPDATE/DELETE through the real capture hook
   from two connections. Hand-built `ColumnarBatch` is allowed only in unit extras.
8. **Layer `host`:** if `requires_in_process_host`, ship real in-process
   registration and call the toy module through SQL.

Host connection details come from the workload `host_runtime` /
`engine_sdk_hints` block and `discover_host_sdk` — do not assume a particular
engine's layout.
