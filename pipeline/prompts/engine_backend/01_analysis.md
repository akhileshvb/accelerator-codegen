# Engine backend analysis

Design the **slot implementations** (S1-S8) of **{{display_name}}**
(`{{engine_name}}`) for the **fixed runtime skeleton**
(`substrate/aux/runtime/`; the runtime flow is substrate and frozen).
From-scratch: substrate + this engine's SDK hints +
`pipeline/templates/engine_backend/`. Do **not** read or copy any other
`backends/{engine}/` tree. Do not implement code here.

{{hard_guidelines}}

## What implementation will do after you

One Codex agent per layer in `layers.md`, in order. Each layer **must** mid-loop
MCP `request_gate` `engine_build` then that layer's gate until both pass.
Your analysis is the **roadmap those agents follow** — concise, concrete, no essays.

{{engine_layers_spec}}

## Sketch each layer (concrete, short)

For **every** layer id (`capture`, `epochs`, `scan`, `runtime`, `host`) write
one object. If `host` is not required, set `skipped: true` and leave files empty.

Each object:

| Field | What to put |
|-------|-------------|
| `id` / `gate` / `cmake_target` | Exact ids (`engine_capture`, `test_{{engine_name}}_capture`, …) |
| `native_apis` | 2–6 **exact** SDK/substrate symbols this layer will call |
| `files` | Exact repo-relative paths **this** layer creates (headers, .cpp, harness, CMake) |
| `harness_plan` | **Golden asserts** (values, keys, NULL, abort vs commit). Not “test the store” |
| `gate_loop` | `request_gate engine_build` then `request_gate <gate>` after each increment |
| `skipped` | bool |

Also fill `placement`, `slot_plan` (one entry per slot S1-S8: the engine
mechanism and exact API), `capture_plan.hook`, `epoch_plan`,
`scan_plan.symbol`, `udf_or_extension_host_plan` (or skip),
`contract_tests_plan.by_layer`, `files_to_create`. Keep strings to one or two lines.
`slot_plan` S6 names where each phase cap comes from (engine setting, env override) and how the budget is shared across processes; S8 names the partitioning unit of `ScanPartitioned` (`native_scan_contract.md`), how every partition reads the build snapshot, and where `TableRows` and the cost-model scale tag come from. `epoch_plan` names the engine clocks: the commit epoch (S2, in visibility
order), the reader epoch (S3, exact), the floor (S4, oldest open snapshot, or
none), the build snapshot epoch `b` (S5), and how a snapshot is mapped to an
epoch when the engine has no single clock.

## Required reads

- `substrate/aux/runtime/README.md`, `engine_slots.hpp`, `runtime_host.hpp`,
  `accelerator_runtime.hpp`, `columnar_batch.hpp`
- `pipeline/prompts/shared/engine_interfaces.md` ("Backend contract")
- `substrate/aux/core/aux_types.hpp`
- `pipeline/prompts/engine_backend/layers.md`
- `pipeline/prompts/engine_backend/acceptance_checklist.md`
- `pipeline/prompts/engine_backend/native_scan_contract.md`
- `substrate/aux/runtime/thread_budget.hpp`, `substrate/aux/core/parallel_slices.hpp`, `partitioned_build.hpp` (what modules will do on top of the slots)
- `pipeline/prompts/engine_backend/layer_contracts.md`
- `pipeline/schemas/backend_analysis.schema.json`

`harness_plan` **must** include the value asserts in `layer_contracts.md` for
that layer (capture: two connections, old/new values, one batch per statement,
union of watched columns, rollback; epochs: visibility order, exact reader
epoch, pending window, floor vs readers; scan: snapshot epoch `b` vs a
concurrent commit, barrier, typed chunks; runtime: rebuild under continuous
writes, old readers, placement failure modes; host: SQL golden, NULL,
`Fallback`).

## Engine SDK / host

{{engine_sdk_hints}}

{{engine_host_discovery}}

If `host_runtime.kind` is `docker`, say so in every `gate_loop`: gates build
**inside the container** (ensure + ninja + `docker exec` of the harness). Do not
plan host-side `cmake -B build-release` as the bar.

## Mode / layout

{{engine_generation_mode}}

CMake collect `{{engine_cmake_collect_fn}}`, flag `{{engine_cmake_build_flag}}`.

{{engine_target_paths}}

{{engine_write_paths}}

## Placement (pick one)

{{optional_examples_section}}

## Output

Write `{{artifacts_path}}/backend_analysis.json` (schema
`pipeline/schemas/backend_analysis.schema.json`).

Do not author `test_{{engine_name}}_adversarial.cpp`.
