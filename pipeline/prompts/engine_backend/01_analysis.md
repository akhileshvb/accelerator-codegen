# Engine backend analysis

Design a **native storage** engine adapter for **{{display_name}}** (`{{engine_name}}`) from the **fixed shared substrate**.

This pipeline is **engine-agnostic**: implement only against the substrate contracts below plus this engine's native APIs. Do **not** require sibling engine trees on disk.

{{hard_guidelines}}

## Primary inputs (required)

Read these substrate contracts (do not regenerate them):

- `include/accelerator_codegen/stateful/core/aux_types.hpp` — `AuxEngineServices`, `AuxStateStore`, `AuxQueryModule`, `MutationBatch`, `VisibilityToken`
- `include/accelerator_codegen/stateful/core/aux_registry.hpp` — txn coordinator
- `include/accelerator_codegen/stateful/state/partitioned_state_store.hpp`
- `include/accelerator_codegen/stateful/maintenance/` — cost model + selector
- `include/accelerator_codegen/runtime/native_scan.hpp` — `NativeScanSpec` → portable `DataChunk` IR
- `pipeline/prompts/engine_backend/native_scan_contract.md`
- `pipeline/prompts/engine_backend/agent_boundary.md`
- `pipeline/schemas/backend_analysis.schema.json`

## Engine SDK hints

{{engine_sdk_hints}}

## Store-mode patterns (conceptual — not tied to a specific engine)

{{optional_examples_section}}

If no prior adapters exist in the repo, proceed from substrate + `pipeline/templates/engine_backend/` alone.

## Mode

{{engine_generation_mode}}

## Writable paths for this workload

{{engine_write_paths}}

## Target layout

{{engine_target_paths}}

CMake: collect `{{engine_cmake_collect_fn}}`, flag `{{engine_cmake_build_flag}}` (empty flag = always-on / default lib).

## Output

Write `{{artifacts_path}}/backend_analysis.json` conforming to `pipeline/schemas/backend_analysis.schema.json`:

- `engine_name`, `store_mode` (`typed_in_process` | `serialized_boundary`)
- `native_scan_plan` — native APIs used to stream base tables into `DataChunk`
- `mutation_capture_plan`, `txn_lifecycle_plan`, `visibility_plan`
- `udf_or_extension_host_plan`
- `files_to_create` / `files_to_update` (under the writable paths above)
- `risks`, `fitness_assessment.recommended`

Do not implement code in this stage.
