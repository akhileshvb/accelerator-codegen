# Backend <-> runtime adversarial tests (validation-owned, unseen)

The **validation** agent authors these tests **after** reading the
implementation. The implementation agent must **not** create or soft-pass this
harness. Goal: catch gaps that the known **layer** harnesses did not cover.

## Required deliverable (validation stage only)

| Item | Conventional path |
|------|-------------------|
| Adversarial test source | `backends/{engine}/tests/test_{engine}_adversarial.cpp` |
| CMake target | `test_{engine}_adversarial` |
| CTest name | `engine_{engine}_adversarial` |

Template: `pipeline/templates/engine_backend/test_engine_adversarial.cpp.tpl`  
CMake hint: `pipeline/templates/engine_backend/cmake_adversarial_test.cmake.tpl`

## Process

1. Read the backend under `backends/{engine}/` (engine, state, api; + host if any).
2. Read `backends/{engine}/tests/test_{engine}_*.cpp` layer harnesses — **do not
   duplicate** them; add **new** scenarios.
3. Invent >=4 adversarial cases from the categories below (those that stress
   *this* implementation's weak points).
4. Register CMake, build, run `engine_adversarial` (and re-run layer gates and
   the `slot_*` gates).
5. On fail: fix the **backend** (preferred) or tighten the harness if the test
   was wrong — never weaken asserts to greenwash.
6. Write `{{artifacts_path}}/validation_report.md` listing each case and
   pass/fail evidence.

## Suggested adversarial categories (engine-agnostic)

1. **Capture** — update that changes no watched column; update of a key column;
   delete of a missing key; multi-row statement ordering; a statement touching
   two tracked tables; write from a connection opened *after* registration and
   one opened *before*; capture after an abort does not leak the prior batch;
   an untracked table is ignored; NULL / extreme values (wide decimal,
   empty and very long strings).
2. **Epochs** — a reader opened exactly at the commit boundary (between
   pre-commit and visibility); many short readers during a burst of commits;
   an abort after pre-commit followed by a commit; commit of a transaction
   that wrote two accelerators' tables while a third accelerator is idle;
   floor with a long-lived reader and with none.
3. **Build under writes** — writes landing at every point of a build (before
   the snapshot, during the scan, after install); a build with zero rows; a
   second invalidation during a build; the registration barrier with a long
   writer.
4. **Scan edges** — empty table; all-NULL projected column; projection of zero
   columns well-defined or rejected; a filter that matches nothing; row count
   forcing many chunks and partitions; a missing table fails closed with a
   clear error.
5. **Placement** (shipped worker) — ring full while the worker builds; worker
   killed mid-build and mid-apply; a corrupt message rejected; a stale worker
   reply after restart.
6. **Scale and threads** — a table larger than the natural scan ranges and fewer rows than partitions; `partitions` far above the thread cap; phase caps of 1 and of the full budget through the engine setting and the env override, with the budget shared when several accelerators build or commit at once; `TableRows` after bulk insert / delete / TRUNCATE; keys beyond 2^32, DECIMAL(38) cells and string heaps beyond 4 GB through capture and scan; a writer commit landing between partition starts of a build scan.
7. **Host / registration** (if a host exists) — entry points resolve; bad
   arguments fail closed without corrupting the runtime; unload and reload.

## Hard rules

- Still **no** TPC-H/SEC accelerator dependency.
- Must compile against the **same** enabled SDK / build flag as the layer suite
  (real native path, not a throw stub / client-protocol scan shim).
- Prefer value asserts and state-machine asserts over "didn't segfault".
- Do not delete or dilute existing `test_{engine}_*.cpp` layer harnesses.
- **No soft skip:** unavailable engine/SDK / Docker host -> `Fail` / non-zero
  exit (never `SKIP` then PASS). See `acceptance_checklist.md` (`fail_closed`).
- Keep >=4 distinct adversarial cases; do not substitute hand-built batches
  for hook-driven paths.

### Engine host tooling (engine profile `host_runtime`)

If the engine profile's `host_runtime` names a Docker / attach helper, use it and fail
closed if the orchestrated host is unreachable. Do **not** fall back to a
host-local server SDK soft path.

## Gate (strict)

`engine_adversarial` builds and runs `test_{engine}_adversarial`, re-checks the
backend against `acceptance_checklist.md` (native scan, real hooks, slots only)
and fails on missing source, TODO stubs, soft-skip, or non-zero exit.
