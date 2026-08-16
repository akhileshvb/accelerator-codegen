# Engine backend analysis

Design a **native storage** engine adapter for **{{display_name}}** (`{{engine_name}}`) from the **fixed shared substrate**.

{{hard_guidelines}}

## Primary inputs (required)

Read these substrate contracts (do not regenerate them):

- `include/accelerator_codegen/stateful/core/aux_types.hpp` — `AuxEngineServices`, `AuxStateStore`, `AuxQueryModule`, `MutationBatch`, `VisibilityToken`
- `include/accelerator_codegen/stateful/core/aux_registry.hpp` — txn coordinator
- `include/accelerator_codegen/stateful/state/partitioned_state_store.hpp`
- `include/accelerator_codegen/stateful/maintenance/` — cost model + selector
- `include/accelerator_codegen/runtime/native_scan.hpp` — `NativeScanSpec` → `DataChunk` IR
- `pipeline/prompts/engine_backend/native_scan_contract.md`
- `pipeline/prompts/engine_backend/agent_boundary.md`

## Engine SDK hints

{{engine_sdk_hints}}

## Optional examples (few-shot only — not required on disk)

{{optional_examples_section}}

If example trees are missing, proceed from substrate + templates alone. This pipeline must work for greenfield engines **and** regenerating duckdb/postgres from scratch.

## Mode

{{engine_generation_mode}}

## Output

Write `{{artifacts_path}}/backend_analysis.json` with:

- `engine_name`, `store_mode` (`typed_in_process` | `serialized_boundary`)
- `native_scan_plan` — how base tables are scanned via native APIs
- `mutation_capture_plan`, `txn_lifecycle_plan`, `visibility_plan`
- `udf_or_extension_host_plan`
- `files_to_create` / `files_to_update`
- `risks`, `fitness_assessment.recommended`

Do not implement code in this stage.
