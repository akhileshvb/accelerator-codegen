# Engine backend analysis

Design a **native storage** adapter for **{{display_name}}** (`{{engine_name}}`)
on the **fixed substrate** (aux/txn/store). From-scratch: substrate + this
engine's SDK hints + `pipeline/templates/engine_backend/`. Do **not** read or
copy any other `backends/{engine}/` tree. Do not implement code here.

{{hard_guidelines}}

## What implementation will do after you

One Codex agent per layer in `layers.md`, in order. Each layer **must** mid-loop
MCP `request_gate` `engine_build` then that layer's gate until both pass.
Your analysis is the **roadmap those agents follow** — concise, concrete, no essays.

{{engine_layers_spec}}

## Sketch each layer (concrete, short)

For **every** layer id (`store`, `aux_backend`, `txn`, `mutations`, `scan`,
`runtime`, `visibility`, `host`) write one object. If `host` is not required,
set `skipped: true` and leave files empty.

Each object:

| Field | What to put |
|-------|-------------|
| `id` / `gate` / `cmake_target` | Exact ids (`engine_store`, `test_{{engine_name}}_store`, …) |
| `native_apis` | 2–6 **exact** SDK/substrate symbols this layer will call |
| `files` | Exact repo-relative paths **this** layer creates (headers, .cpp, harness, CMake) |
| `harness_plan` | **Golden asserts** (values, keys, NULL, abort vs commit). Not “test the store” |
| `gate_loop` | `request_gate engine_build` then `request_gate <gate>` after each increment |
| `skipped` | bool |

Also fill `store_mode`, `native_scan_plan.symbol`, `mutation_capture_plan.hook`,
`txn_lifecycle_plan.api`, `visibility_plan`, `udf_or_extension_host_plan` (or skip),
`contract_tests_plan.by_layer`, `files_to_create`. Keep strings to one or two lines.

## Required reads

- `substrate/aux/core/aux_types.hpp`
- `substrate/aux/core/aux_registry.hpp`
- `substrate/aux/state/partitioned_state_store.hpp`
- `pipeline/prompts/engine_backend/layers.md`
- `pipeline/prompts/engine_backend/acceptance_checklist.md`
- `pipeline/prompts/engine_backend/layer_contracts.md`
- `pipeline/schemas/backend_analysis.schema.json`

`harness_plan` **must** include the value asserts in `layer_contracts.md` for
that layer (store: CAS + token filter + key-from-publish, no `RegisterQueryKey`;
aux_backend: `AttachServices` + commit via coordinator, not harness
`TryPublish`; txn: engine txn API, not C++ `OnCommit` as the proof).

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

## Store-mode (pick one)

{{optional_examples_section}}

## Output

Write `{{artifacts_path}}/backend_analysis.json` (schema
`pipeline/schemas/backend_analysis.schema.json`).

Do not author `test_{{engine_name}}_adversarial.cpp`.
