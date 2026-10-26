# Engine backend implementation — layer `hostctl`

Implement **only** the host script of **{{display_name}}** (`{{engine_name}}`). Do **not**
start a later layer. Every later layer's gate reaches the engine through this script.


## This layer

- **id:** `{{layer_id}}`
- **gate:** `{{layer_gate}}`  ← MCP `request_gate` this name
- **harness:** `{{layer_harness}}`
- **host_runtime.kind:** `{{host_runtime_kind}}`

Spec: the host contract below (binding) and the `hostctl` section of
`pipeline/prompts/engine_backend/docs/layer_contracts.md`. Checklist rules owned by this layer:
`{{layer_rules}}` (`acceptance_checklist.md`).

## Engine profile (this layer's sections)

Facts and hazards of the engine, not instructions; `n/a` is a declared decision.

{{engine_profile}}
## Host contract (binding)

{{host_contract}}

## What to write

`backends/{{engine_name}}/host.sh` (helpers under `backends/{{engine_name}}/host/`) implementing
every verb above. Kept code (gates, orchestrator, scripts) reaches the engine only through it, so it
must work with nothing but the contract's environment: `ACCEL_ENGINE_INSTANCE`,
`ACCEL_ENGINE_STATE_DIR`, `ACCEL_BUILD_DIR`, `ACCEL_DATASET_DIR`, `ACCEL_CMAKE_ARGS`.

Source every engine fact (packaging, image or install, server and client, ports, plugin directory,
how an extension is loaded) from the engine profile. Do not read another engine's `backends/` tree.

- **Instances.** Derive container, socket, port and database names from `ACCEL_ENGINE_INSTANCE`;
  keep all data under `ACCEL_ENGINE_STATE_DIR`; pick a free port. `up` is idempotent and writes
  `$ACCEL_ENGINE_STATE_DIR/env`; `down` removes everything the instance created. If the profile
  says the engine is embedded (no server), `up`/`down` manage only its database file.
- **Dataset.** `load` reads `$ACCEL_DATASET_DIR/<table>.tbl` (`|`-separated, a trailing `|` on every
  line) with the engine's bulk loader, after creating the schema from the given SQL file; adapt types
  only where the profile's hazards require it (text collation and padding), never the data.
- **Build.** `build` compiles the backend plus the named targets with the engine's toolchain (in the
  engine's own build environment when the profile says so) and installs or loads whatever the backend
  has so far. It configures the build dir with `$ACCEL_CMAKE_ARGS` (reconfiguring when the cache
  differs); nothing else configures it, and you never run `cmake -S/-B` on it yourself. Later layers
  add what is built (capture hooks, entry points, the API) and extend `build` where installing or
  loading needs more.
- **Run.** `run <target> [args]` executes a built binary where it reaches the instance, with the
  `env` file applied; a target that is not a built file runs as a PATH command (for example `ctest`).
  Connection details come from `env`, never from arguments.
- **Slot.** No harness exists yet, so `slot <scenario> <seconds>` exits 1 with "not wired yet" for
  every scenario (77 only for a mechanic the profile declares n/a). The validation stage wires each
  scenario to its driver.

Also write the minimal `{{engine_cmake_entry}}`, so `engine_build` is real from this layer on:
the `accel_backend_{{engine_name}}` INTERFACE target (engine SDK include directories and client
libraries from the profile) and the test glob loop of
`pipeline/templates/engine_backend/backend.cmake.tpl`. Leave out `accel_backend_library(...)` and the
module: those name sources that later layers create, and they add them. The root `CMakeLists.txt`
includes the file when `{{engine_cmake_build_flag}}` is ON; do not edit `CMakeLists.txt` or `cmake/**`.

Keep the script small and readable: one function per verb, no per-query or per-workload names.

## Gates

After a first working increment call MCP tools on server `accel_pipeline` (tools, not resources):
`run_engine_build()` (`engine_build`: `host.sh up` + `build`, the build dir configured with
`$ACCEL_CMAKE_ARGS` and the backend switch ON) and `run_layer_gate()` (`{{layer_gate}}`: `info`,
`up`, `load` twice, `sql`, `down`). Fix from the traces; do not end until both are `status=pass`.
If you end with a post-stage gate red, you are re-invoked with its notes and log tail.
Shell-debug through the same `host.sh` verbs.

## Mode

{{engine_generation_mode}}

{{engine_host_discovery}}

## Writable / target paths

{{engine_write_paths}}

## Invariants

1. Do not modify `substrate/**`, `cmake/**` or `CMakeLists.txt`.
2. Do not read or copy another engine's tree.
3. Nothing outside `backends/{{engine_name}}/` names the engine's server, client, container or data layout.
4. Fail closed: a verb that cannot do its job exits non-zero.
