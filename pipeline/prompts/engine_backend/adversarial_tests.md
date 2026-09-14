# Engine ↔ substrate adversarial tests (validation-owned, unseen)

The **validation** agent authors these tests **after** reading the
implementation. The implementation agent must **not** create or soft-pass this
harness. Goal: catch gaps that the known **layer** harnesses did not cover.

## Required deliverable (validation stage only)

| Item | Conventional path |
|------|-------------------|
| Adversarial test source | `tests/engine_backend/test_{engine}_adversarial.cpp` |
| CMake target | `test_{engine}_adversarial` |
| CTest name | `engine_{engine}_adversarial` |

Template: `pipeline/templates/engine_backend/test_engine_adversarial.cpp.tpl`  
CMake hint: `pipeline/templates/engine_backend/cmake_adversarial_test.cmake.tpl`

## Process

1. Read the adapter under `runtime/{engine}` + `stateful/{engine}` (+ host if any).
2. Read `tests/engine_backend/test_{engine}_*.cpp` layer harnesses — **do not duplicate** them;
   add **new** scenarios.
3. Invent ≥4 adversarial cases from the categories below (pick those that stress
   *this* implementation’s weak points).
4. Register CMake, build, run `engine_adversarial` (and re-run layer gates).
5. On fail: fix the **adapter** (preferred) or tighten harness if the test was
   wrong — never weaken asserts to greenwash.
6. Write `{{artifacts_path}}/validation_report.md` listing each adversarial case
   and pass/fail evidence.

## Suggested adversarial categories (engine-agnostic)

Choose concrete instances appropriate to the engine’s native APIs:

1. **Scan edge cases** — empty table; all-NULL projected column; type coercion
   corners; projection of zero columns rejected or well-defined; filter that
   matches nothing; large row count forcing multi-chunk; invalid relation /
   missing table fails closed with a clear error.
2. **Mutation adversarial** — update with no-op identical values; delete of
   missing key; batched multi-row capture ordering; capture after abort does
   not leak prior batch; wrong table id ignored/rejected.
3. **Txn poisoning** — Abort mid-flight then Commit attempt; double Commit;
   Commit without PreCommit if API forbids it; nested/overlapping txn tokens if
   the engine model allows.
4. **Visibility races (logical)** — reader token taken before commit must not
   observe published image; after invalidate, old token fails `CheckValidity`.
5. **Store integrity** — corrupt / truncated serialized blob rejected under
   `serialized_boundary`; publish of empty image; overwrite same partition;
   read after process-local reset policy.
6. **Host / registration smoke** (if host exists) — entrypoints resolve; bad
   args fail closed without corrupting coordinator.

## Hard rules

- Still **no** TPC-H/SEC accelerator dependency.
- Must compile against the **same** enabled SDK / build flag as the contract
  suite (real native path, not throw stub / not client-protocol scan shim).
- Prefer value asserts and state-machine asserts over “didn’t segfault”.
- Do not delete or dilute existing `test_{engine}_*.cpp` layer harnesses to make validation pass.
- **No soft skip:** unavailable engine/SDK / Docker host → `Fail` / non-zero
  exit (never `SKIP` then PASS). See `acceptance_checklist.md` (`fail_closed`).
- Keep ≥4 distinct adversarial cases; do not re-introduce hand-built-only
  mutation coverage as a substitute for hook-driven paths.
- Re-check the closed bar in
  `pipeline/prompts/engine_backend/acceptance_checklist.md` — adversarial cases
  must still exercise **native** scan / real hooks, not client-protocol stand-ins.

### Engine host tooling (from workload SDK hints)

If this engine’s SDK hints name a Docker / attach helper (e.g. DiscoverTools with
`ACCEL_*_DOCKER=1`), use that helper and fail closed if the orchestrated host is
unreachable. Do **not** fall back to a host-local server SDK soft path.

## Gate (strict)

`engine_adversarial` builds and runs `test_{engine}_adversarial`, re-checks
adapter architecture (native scan + real hooks), and fails on missing source,
TODO stubs, soft-skip, or non-zero exit.
