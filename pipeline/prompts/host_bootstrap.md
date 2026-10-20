# Stage: Query host bootstrap (once per engine)

You are bootstrapping the **query-facing host** for **`{{engine_name}}`** so later per-query accelerator workstreams can register and differential-test without racing.

`engine_name` is **required**. Do not default to another engine or invent a portable chunk-based registration path.

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
| Query tree | `accelerators/{bench}/q*/{{engine_name}}/` |
| Registries | the **registration** role of `backends/{{engine_name}}/api/README.md` (entries registered from each query's directory) |

Prefer these conventions. `backends/{{engine_name}}/**` is **read-only** to this stage (the `engine_backend` run owns it; query runs never write it). If the backend lacks a generic surface you need, append a request (`{"benchmark", "query", "helper", "reason"}`, no query names in `helper`) to `pipeline/artifacts/backend_additions/{{engine_name}}.jsonl` and report it; never create a parallel tree.

## Ordered plan (minimize time-to-gate-pass)

### Phase 1 — required for `host_bootstrap` gate (do this first)

1. **Locate existing host**
   - Read `{{register_path}}` (and sibling files under `backends/{{engine_name}}/api/`, `backends/{{engine_name}}/engine/`, `backends/{{engine_name}}/state/`).
   - Read the backend's own harnesses (`backends/{{engine_name}}/tests/`) and `backends/{{engine_name}}/host.sh` (`info` lists the verbs) as the templates.
   - Reuse this engine’s `engine_backend` adapters for **mutation / txn / store / tooling** and any **engine-local** rebuild scan helper. Do **not** invent a parallel `backends/{{engine_name}}2/`.

2. **SQL entrypoints (backend registries, read-only here)**
   - The backend (`backends/{{engine_name}}/api/`) provides generic **registries** + dispatch so each query registers its base + aux call paths from its own directory (`{{register_path}}`) via static registrars, through **this host’s** native APIs + `AuxQueryModule`. Check they exist and link; the backend names no query and builds with every `accelerators/<bench>/q<N>/` removed. A missing registry is a backend-addition request for the `engine_backend` run, not an edit here.
   - You do **not** need a full TPC-H Q{{query_number}} kernel here — registries + generic dispatch that **link and load** (with zero or one registered query) are enough for bootstrap.
   - Never wire this host through another engine's registration or dataset format.

   The backend implements the slots S1-S8 (`engine_interfaces.md`, "Adapter responsibilities"); the runtime flow comes from `substrate/aux/runtime/`. Check that the slot implementations under `engine/` (S1, S5, S8), `state/` (S2-S4) and `api/` (S6, S7) link into the host. Missing slot behavior is a backend-addition request.

3. **Differential harness**
   - Create `{{differential_source}}` if missing.
   - TPC-H lives in **`{{engine_name}}`'s native storage** (`host.sh up`, `host.sh load`);
     another engine may be a reference oracle or loader source only — never the scan host.
   - Differential semantics: load TPC-H into `{{engine_name}}`, run reference SQL vs accelerator SQL for a query id,
     assert ordered full-result match (or a documented minimal smoke).
   - Declare the target in `backends/{{engine_name}}/backend.cmake` (never the root `CMakeLists.txt`).
   - **Build:** `backends/{{engine_name}}/host.sh build {{differential_binary}}`.
   - CLI: {{differential_command}}

4. **Pass the gate**
   - Call MCP `request_gate` with **`host_bootstrap`**.
   - **Do not finish Phase 1 until it passes.** The gate only requires the differential target to **build**; fix link/CMake/register issues until it does.

### Phase 2 — after Phase 1 passes (same stage if turns remain)

Gate surfaces under engine-native names (host `{{engine_name}}`, export `ACCEL_ENGINE_NAME={{engine_name}}`):

- `eval/tests/test_{{engine_name}}_aux_multi_connection_stress.cpp` → ctest `{{multi_conn_ctest_regex}}`
- `eval/tests/test_{{engine_name}}_cross_accel.cpp` (or peer naming) → ctest `{{cross_accel_ctest_regex}}`
- Runtime gate harnesses named in `pipeline/gates/README_runtime_gates.md` (stale reads, capture completeness, rebuild under writes) belong to the backend's tests; do not stub them here.
- Bench / calibrate hooks driven by `ACCEL_ENGINE_NAME` + host work dir when straightforward

If turns are short after Phase 1, prefer a clean gate pass over incomplete Phase 2 stubs. Per-query stages can finish missing harnesses, but shared CMake + registration helpers should already exist.

## Do not

- Implement a full per-query TPC-H kernel / optimizer work here.
- Invent a parallel adapter tree, or reimplement runtime flow / thread pools (substrate owns them).
- Use another engine's registration or dataset format as the host for `{{engine_name}}`.
- Rename targets away from `{{differential_binary}}` / `{{differential_source}}` (gates look these up).
- Require a portable chunk type as the registration contract.

## Exit criterion

`request_gate` **`host_bootstrap`** → **pass** (differential binary built). Then stop or continue Phase 2.

If you finish without a green gate, the orchestrator re-runs this stage with
the gate `notes` + `log_tail`. Fix from that trace; do not ignore it.

The pass bar is `host.sh build` of the differential target, not a bare local `cmake --build`.

After this stage, a separate critic may fail a green *build* (stub harness,
wrong target name, second adapter tree) and re-invoke this stage.

After bootstrap, parallel per-query workers only append query-specific registration + kernels under `.../q*/{{engine_name}}/`.
