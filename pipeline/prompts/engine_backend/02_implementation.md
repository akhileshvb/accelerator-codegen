# Engine backend implementation

Implement or update the **{{display_name}}** (`{{engine_name}}`) adapter.

{{hard_guidelines}}

{{codex_tooling}}

## Mode

{{engine_generation_mode}}

- **generate_from_scratch**: emit adapters from templates + substrate.
- **update_existing**: scaffold/CMake/registration/contract drift only — do not wipe a working backend unless analysis requires it.

## Analysis

Full analysis: `{{artifacts_path}}/backend_analysis.json`

## Target paths

{{engine_target_paths}}

## Templates

Copy/adapt from `pipeline/templates/engine_backend/` (scan adapter, aux backend, state store, mutation capture, cmake collect).

## Contracts

- Base tables: **native storage APIs only** (`NativeScanSpec` → `DataChunk`)
- Aux state: **custom storage OK / preferred**
- Implement `AuxEngineServices`, `AuxStateStore`, mutation → `MutationBatch`, txn lifecycle → coordinator
- Do **not** modify fixed substrate (`core` / `state` / `maintenance`)

## Optional examples

{{optional_examples_section}}

Build with engine CMake flag when applicable; gates run after this stage.
