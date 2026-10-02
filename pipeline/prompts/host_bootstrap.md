# Stage: Query host bootstrap (once per engine)

You are bootstrapping the **query-facing host** for **`{{engine_name}}`** so later per-query accelerator workstreams can register and differential-test without racing.

`engine_name` is **required**. Do not default to duckdb or invent a portable `DataChunk` registration path.

{{hard_guidelines}}

{{codex_tooling}}

{{engine_host_discovery}}

## Paths (from `engine_host` — do not invent alternate names)

| Role | Path / name |
|------|-------------|
| Engine id | `{{engine_name}}` |
| SQL / registration surface | `{{register_path}}` |
| Aux registration surface | `{{aux_register_path}}` |
| Differential source | `{{differential_source}}` |
| Differential CMake target / binary | `{{differential_binary}}` |
| Multi-conn ctest (phase 2) | `{{multi_conn_ctest_regex}}` |
| Cross-accel ctest (phase 2) | `{{cross_accel_ctest_regex}}` |
| Build dir | `{{build_dir}}` |
| Dataset / SDK env | `{{dataset_env}}` |
| Query tree | `src/generated/{bench}/q*/{{engine_name}}/` |
| Catalog registry | `src/generated/catalog/{{engine_name}}/query_catalog.{hpp,cpp}` (entries registered from each query's directory) |

Prefer these conventions. If an engine already uses a slightly different file under `src/generated/{{engine_name}}/` or `runtime|stateful/{{engine_name}}/`, extend that file instead of creating a parallel tree.

## Ordered plan (minimize time-to-gate-pass)

### Phase 1 — required for `host_bootstrap` gate (do this first)

1. **Locate existing host**
   - Read `{{register_path}}` (and sibling files under `src/generated/{{engine_name}}/`, `src/runtime/{{engine_name}}/`, `src/stateful/{{engine_name}}/`).
   - Skim duckdb reference surfaces only as templates (structure, not DataChunk IR):
     - `tests/aux_state/test_tpch_shared_differential.cpp`
     - `tests/aux_state/test_aux_multi_connection_stress.cpp` / `test_cross_accel_correctness.cpp` (phase 2)
     - Nearby `add_executable(test_tpch_shared_differential …)` in root `CMakeLists.txt`
   - Reuse this engine’s `engine_backend` adapters for **mutation / txn / store / tooling** and any **engine-local** rebuild scan helper. Do **not** invent `runtime/{{engine_name}}2`. Do **not** require `ScanNativeBaseTable` → DataChunk.

2. **SQL entrypoints (shared, append-only)**
   - In the backend (`src/generated/{{engine_name}}/`, `runtime|stateful/{{engine_name}}/`), add generic **registries** + dispatch so each later query registers its base + aux call paths from its own directory (`{{register_path}}`) via static registrars, through **this host’s** native APIs + `AuxQueryModule`. The backend must name no query and must still build with every `src/generated/<bench>/q<N>/` removed.
   - You do **not** need a full TPC-H Q{{query_number}} kernel here — registries + generic dispatch that **link and load** (with zero or one registered query) are enough for bootstrap.
   - Never wire this host through DuckDB `RegisterAcceleratorFunctions` or `*.duckdb` as the execution host when `{{engine_name}}≠duckdb`.

3. **Differential harness**
   - Create `{{differential_source}}` if missing.
   - For **postgres**: TPC-H lives in **Postgres-native tables** (Docker DB
     `accel_tpch_test` via `scripts/setup_accel_postgres_tpch_test_db.sh`).
     DuckDB may be a reference oracle/loader source only — not the scan host.
   - Mirror duckdb differential semantics for **this** engine: load TPC-H into
     `{{engine_name}}`, run reference SQL vs accelerator SQL for a query id,
     assert ordered full-result match (or a documented minimal smoke).
   - Wire CMake next to peer engine targets (`add_executable({{differential_binary}} …)`).
   - **Build (postgres / Docker):**
     ```bash
     bash scripts/ensure_accel_pg_test.sh
     ACCEL_PG_NINJA_TARGETS={{differential_binary}} bash scripts/build_accel_postgres_extension.sh
     docker exec accel_pg_test test -x /accel-build/accel/{{differential_binary}}
     ```
     Other engines: `cmake --build {{build_dir}} --target {{differential_binary}} -j"$(nproc)"`.
   - CLI: {{differential_command}}

4. **Pass the gate**
   - Call MCP `request_gate` with **`host_bootstrap`**.
   - **Do not finish Phase 1 until it passes.** The gate only requires the differential target to **build**; fix link/CMake/register issues until it does.

### Phase 2 — after Phase 1 passes (same stage if turns remain)

Replicate duckdb gate surfaces under engine-native names (same semantics, host `{{engine_name}}`, export `ACCEL_ENGINE_NAME={{engine_name}}`):

- `tests/aux_state/test_{{engine_name}}_aux_multi_connection_stress.cpp` → ctest `{{multi_conn_ctest_regex}}`
- `tests/aux_state/test_{{engine_name}}_cross_accel.cpp` (or peer naming) → ctest `{{cross_accel_ctest_regex}}`
- Bench / calibrate hooks driven by `ACCEL_ENGINE_NAME` + host work dir when straightforward

If turns are short after Phase 1, prefer a clean gate pass over incomplete Phase 2 stubs. Per-query stages can finish missing harnesses, but shared CMake + registration helpers should already exist.

## Do not

- Implement a full per-query TPC-H kernel / optimizer work here.
- Invent a parallel adapter tree.
- Use DuckDB registration / `*.duckdb` as the host for `{{engine_name}}` when it is not duckdb.
- Rename targets away from `{{differential_binary}}` / `{{differential_source}}` (gates look these up).
- Require portable `DataChunk` / `NativeScanSpec` as the registration contract.

## Exit criterion

`request_gate` **`host_bootstrap`** → **pass** (differential binary built). Then stop or continue Phase 2.

If you finish without a green gate, the orchestrator re-runs this stage with
the gate `notes` + `log_tail`. Fix from that trace; do not ignore it.

**Postgres:** the gate is Docker Ninja (`/accel-build/accel/{{differential_binary}}`).
Host `cmake --build build-release` / GNU make is not the pass bar and will
mislead you (`No rule to make target`).

After this stage, a separate critic may fail a green *build* (stub harness,
wrong target name, second adapter tree) and re-invoke this stage.

After bootstrap, parallel per-query workers only append query-specific registration + kernels under `.../q*/{{engine_name}}/`.
