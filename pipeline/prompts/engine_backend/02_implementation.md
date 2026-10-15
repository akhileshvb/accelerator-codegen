# Engine backend implementation — layer `{{layer_id}}`

Implement **only** **{{layer_title}}** (`{{layer_id}}`) for **{{display_name}}**
(`{{engine_name}}`). Do **not** start a later layer. Do **not** author
`test_{{engine_name}}_adversarial.cpp`.

{{hard_guidelines}}

{{codex_tooling}}

## This layer

- **id:** `{{layer_id}}`
- **gate:** `{{layer_gate}}`  ← MCP `request_gate` this name
- **harness:** `{{layer_harness}}`
- **CMake target:** `{{layer_cmake_target}}`
- **host_runtime.kind:** `{{host_runtime_kind}}`

Spec: `pipeline/prompts/engine_backend/layers.md` (this row only).
Checklist rules owned by this layer: `acceptance_checklist.md`.
Substrate contracts (this layer’s section is binding):
`pipeline/prompts/engine_backend/layer_contracts.md`.
Analysis sketch: `{{artifacts_path}}/backend_analysis.json` → `layers[id={{layer_id}}]`.
If the sketch contradicts `layer_contracts.md` or the checklist, **follow the
contracts**.

## Required reads (before writing code)

- `pipeline/prompts/engine_backend/layer_contracts.md` (section `{{layer_id}}`)
- `pipeline/prompts/engine_backend/acceptance_checklist.md`
- `substrate/aux/core/aux_types.hpp`
- `substrate/aux/state/versioned_aux_store.hpp`
- `substrate/aux/core/aux_registry.hpp` (from `aux_backend` onward)
- Matching stub under `pipeline/templates/engine_backend/` — keep
  `aux_state::engine::{{engine_name}}`. Do not put adapter types in `core::`.

## Prior layers (reuse; minimal fix if blocked)

{{prior_layers}}

Reuse prior-layer objects. Do **not** rewrite them, restyle them, or reopen
their design for taste.

**Exception:** if this layer cannot meet *its* contract because a prior layer
has a real bug or design flaw (wrong substrate API, untested path that later
code will hit, inverted partition keys, missing `AttachServices`, etc.), make
the **smallest** patch to that prior adapter **and** keep its harness honest:

1. Touch only the files that must change. No drive-by refactors.
2. If the prior harness missed the hole, add the missing assert there — do not
   weaken it.
3. `request_gate` `engine_build`, then **that prior layer’s gate**, then
   continue this layer’s gate until both (all) are green.
4. Do **not** start a later layer to work around the hole.

## Gates — steer with them (non-negotiable)

Do **not** implement the whole layer and then hope. After each small increment
(new file, CMake wiring, or a real assert), call MCP tools on server
`accel_pipeline`. These are **tools**, not resources — do **not**
`list_mcp_resources`. Preferred calls (zero-arg):

- `run_engine_build()` → official `engine_build`
- `run_layer_gate()` → official `{{layer_gate}}` for this `impl_*` stage

Or `request_gate` with `gate_name` `engine_build` / `{{layer_gate}}`.

| When | Gate | What it does |
|------|------|----------------|
| After CMake / adapter TUs that should link | `engine_build` | Host cmake **or** docker `ensure_script` (kind=`{{host_runtime_kind}}`) |
| After harness exists and should run | `{{layer_gate}}` | Builds **`{{layer_cmake_target}}`** and runs **`{{layer_harness}}`** |

Suggested cadence:

1. Write the harness file (even if `TestLayer` still `Fail`s) + CMake target.
2. MCP `run_engine_build()`. If fail: read `failure_summary` / `log_tail`, fix, retry.
3. MCP `run_layer_gate()`. Missing harness / missing ninja target / failed assert is the signal.
4. Fill real value asserts; call the MCP gates again until **both** are `status=pass`.
5. **Do not end** until both pass. No “mostly done”, no later layers.

If you end while a post-stage gate is still red, the orchestrator re-invokes
**this same layer** with that gate’s `notes` and `log_tail`. Treat that as the
next increment: fix from the trace, MCP-gate again, do not start a later
layer. This applies to every `impl_*` stage.

### Docker host (`host_runtime.kind=docker`)

This workload builds **inside the container**. `request_gate` already:

1. Runs `{{host_ensure_script}}` (starts/attaches the engine Docker host)
2. Runs `{{host_build_script}}` with this layer's cmake target in the ninja-targets env
3. `docker exec`s `{{layer_cmake_target}}` in the container

**Do not** `cmake -B build-release` on the developer machine as the correctness bar.
If you shell-debug, use the same ensure/build scripts; still finish via `request_gate`.

### Host cmake (`host_runtime.kind=host_cmake`)

Ad-hoc `cmake --build … --target {{layer_cmake_target}}` is OK **between** MCP
gate calls. Official pass/fail is still `request_gate`.

## Mode

{{engine_generation_mode}}

{{engine_host_discovery}}

## Writable / target paths

{{engine_target_paths}}

{{engine_write_paths}}

## Templates

- Harness: `pipeline/templates/engine_backend/test_engine_layer.cpp.tpl` → `{{layer_harness}}`
- CMake: `pipeline/templates/engine_backend/cmake_layer_test.cmake.tpl`
- Stubs as needed: `pipeline/templates/engine_backend/` (`state_store`, `aux_backend`, `mutation_capture`)

Wire collect `{{engine_cmake_collect_fn}}` / flag `{{engine_cmake_build_flag}}`.
Use `cmake_layer_test.cmake.tpl` for the harness executable (gated by the
engine build flag). Add adapter `.cpp` files through the collect function —
do not dump an unguarded `add_executable` at the bottom of `CMakeLists.txt`.

{{layer_contracts}}

## Invariants

1. Do not modify `core` / `state` / `maintenance`.
2. Do not read or copy another engine's tree.
3. One `AuxEngineServices` (the aux backend). Scans are a separate object.
4. Toy module/scan only — no generated query accelerators.
5. Fail closed if the SDK/host cannot start.

After this layer’s gates pass, a **separate cheap critique agent** reviews the
adapter + harness against `layer_contracts.md`. A green gate is not done if
the critic finds an untested contract path. You will be re-invoked with its
feedback — do not greenwash the harness.
