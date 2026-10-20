# Host script layer

Write `backends/{{engine_name}}/host.sh` (and helpers under `backends/{{engine_name}}/host/`)
implementing every verb of `pipeline/prompts/engine_backend/host_contract.md`: `info`, `up`,
`down`, `load`, `build`, `sql`, `run`, `slot`. Kept code (gates, orchestrator, scripts) reaches the
engine only through this script, so it must work with nothing but the contract's environment:
`ACCEL_ENGINE_INSTANCE`, `ACCEL_ENGINE_STATE_DIR`, `ACCEL_BUILD_DIR`, `ACCEL_DATASET_DIR`.

Source every engine fact (packaging, image or install, server and client, ports, plugin directory,
how an extension is loaded, how mutations are captured) from the engine profile above. Do not read
another engine's `backends/` tree.

- **Instances.** Derive container, socket, port and database names from `ACCEL_ENGINE_INSTANCE`;
  keep all data under `ACCEL_ENGINE_STATE_DIR`; pick a free port. `up` is idempotent and writes
  `$ACCEL_ENGINE_STATE_DIR/env`; `down` removes everything the instance created. If the profile
  says the engine is embedded (no server), `up`/`down` manage only its database file.
- **Dataset.** `load` reads `$ACCEL_DATASET_DIR/<table>.tbl` (`|`-separated, a trailing `|` on every
  line) with the engine's bulk loader, after creating the schema from the given SQL file; adapt types
  only where the profile's hazards require it (text collation and padding), never the data.
- **Build.** `build` compiles the backend plus the named targets with the engine's toolchain (in the
  engine's own build environment when the profile says so), installs or loads the result, and
  registers the SQL-visible entry points and the capture hooks on every table of the schema.
  It configures the build dir with `$ACCEL_CMAKE_ARGS` (reconfiguring when the cache differs);
  nothing else configures it, and you never run `cmake -S/-B` on it yourself.
- **Run.** `run <target> [args]` executes a built binary where it reaches the instance, with the
  `env` file applied; a target that is not a built file runs as a PATH command (for example `ctest`).
  Connection details come from `env`, never from arguments.
- **Slot.** No harness exists yet in this first layer, so `slot <scenario> <seconds>` exits 1 with
  "not wired yet" for every scenario (77 only for a mechanic the profile declares n/a). The
  validation stage wires each scenario to its driver.

Keep the script small and readable: one function per verb, no per-query or per-workload names.
