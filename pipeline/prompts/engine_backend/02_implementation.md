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

## Engine profile (this layer's sections)

Facts and hazards of the engine, not instructions; `n/a` is a declared decision.

{{engine_profile}}
Build from these facts and the contracts. The backend tree starts empty: a file you see named in
some document but absent from the tree does not exist, and is never a reason to stop. Write what
the layer needs. Where the analysis calls a mechanic unavailable, the layer contracts give the design
that works without it (no pre-commit hook, no exact reader epoch, no shareable snapshot): build that.
A layer always ships code and its harness; "blocked" is not a result.
You run unattended: nobody answers a question, and a turn that ends asking for scope or
permission changes nothing. A layer owns its contract, not a set of files: the wiring the
contract needs (plugin or extension registration, SQL that `host.sh` installs, a binding into
another layer's object) is this layer's work. Make it and keep every harness green.
A missing SDK, header or package is not a blocker either: the backend owns its build
environment (`host.sh up`/`build` and the image it sets up). Fetch or install what the profile's
`identity.sdk_headers` names, or declare in a backend header the stable ABI subset the backend uses.
A profile field given as `{na: reason}` declares that mechanic absent on this engine: it is not a requirement here. Do not fail, implement or test it; only make sure nothing depends on it.

## Required reads (before writing code)

- `pipeline/prompts/engine_backend/layer_contracts.md`: section `{{layer_id}}` and "Requirements every backend meets" (the requirements that name this layer, the registries included)
- `pipeline/prompts/engine_backend/acceptance_checklist.md`
- `pipeline/prompts/shared/engine_interfaces.md` ("Backend contract": slot contracts)
- `substrate/aux/runtime/README.md`, `engine_slots.hpp`, `runtime_host.hpp`,
  `accelerator_runtime.hpp`, `columnar_batch.hpp` (and `wire.hpp`,
  `thread_budget.hpp` where the layer touches them)
- Layer `scan`: `pipeline/prompts/engine_backend/native_scan_contract.md` (the `ScanPartitioned` surface modules call); layer `runtime`: `substrate/aux/runtime/thread_budget.hpp` (phase caps) and `AuxCostModelLookup` in `substrate/aux/core/aux_cost_model_path.hpp`
- Layer `api`: `pipeline/prompts/engine_backend/api_contract.md` and `host_contract.md` (binding)
- `eval/tests/test_aux_runtime.cpp` — the toy `SumModule` to copy for the harness
- Matching stub under `pipeline/templates/engine_backend/` — keep
  `aux_state::engine::{{engine_name}}`. Do not put backend types in `core::`.

You implement **slots only**. The runtime flow (commit order, gaps, catch-up,
builds, versions, thread pool) is substrate: never re-implement or work around it.

## Prior layers (reuse; minimal fix if blocked)

{{prior_layers}}

Reuse prior-layer objects. Do **not** rewrite them, restyle them, or reopen
their design for taste.

**Exception:** if this layer cannot meet *its* contract because a prior layer
has a real bug or design flaw (wrong epoch order, an untested path that later
code will hit, a capture that misses a connection, etc.), make
the **smallest** patch to that prior backend layer **and** keep its harness honest:

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
| After CMake / backend TUs that should link | `engine_build` | `host.sh build` (kind=`{{host_runtime_kind}}`) |
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

1. Runs `backends/{{engine_name}}/host.sh up` (starts or attaches the engine host)
2. Runs `backends/{{engine_name}}/host.sh build {{layer_cmake_target}}`
3. Runs `backends/{{engine_name}}/host.sh run {{layer_cmake_target}}`

**Do not** `cmake -B build-release` on the developer machine as the correctness bar.
If you shell-debug, use the same `host.sh` verbs; still finish via `request_gate`.

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
- CMake: `pipeline/templates/engine_backend/backend.cmake.tpl` → `{{engine_cmake_entry}}`
- Stubs as needed: `pipeline/templates/engine_backend/` (`host`, `capture`, `scan`)

Write `{{engine_cmake_entry}}` to `cmake/Backend.cmake`'s interface (see
`backend.cmake.tpl`); the root includes it when `{{engine_cmake_build_flag}}` is ON.
List new backend `.cpp` files there; harnesses `tests/test_{{engine_name}}_<layer>.cpp`
register themselves through its glob. Do not edit `CMakeLists.txt` or `cmake/**`.

{{layer_contracts}}

## Invariants

1. Do not modify `substrate/aux/**`.
2. Do not read or copy another engine's tree.
3. Slots only: no runtime flow, no edits to `substrate/aux/**`.
4. Toy module only — no generated query accelerators.
5. Fail closed if the SDK/host cannot start.

After this layer’s gates pass, a **separate cheap critique agent** reviews the
backend + harness against `layer_contracts.md`. A green gate is not done if
the critic finds an untested contract path. You will be re-invoked with its
feedback — do not greenwash the harness.
