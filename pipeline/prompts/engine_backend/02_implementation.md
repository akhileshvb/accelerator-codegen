# Engine backend implementation

Implement or update the **{{display_name}}** (`{{engine_name}}`) adapter.

{{hard_guidelines}}

{{codex_tooling}}

## Mode

{{engine_generation_mode}}

- **generate_from_scratch**: emit adapters from templates + substrate under the target layout.
- **update_existing**: scaffold / CMake / registration / contract drift only — do not wipe a working backend unless analysis requires it.

## Analysis

Full analysis: `{{artifacts_path}}/backend_analysis.json`

## Target paths

{{engine_target_paths}}

## Writable paths

{{engine_write_paths}}

## Templates

Copy/adapt from `pipeline/templates/engine_backend/` (headers + `.cpp` stubs + cmake collect). Substitute template placeholders with this engine's id (`{{engine_name}}` / `{{EngineName}}`).

## Contracts (engine-agnostic)

1. Base tables: **native storage APIs only** (`NativeScanSpec` → portable `DataChunk` IR).
2. Aux state: **custom storage OK / preferred** behind `AuxStateStore`.
3. Implement `AuxEngineServices`, `AuxStateStore`, mutation → `MutationBatch`, txn lifecycle → coordinator.
4. Wire CMake via `{{engine_cmake_collect_fn}}` and optional `{{engine_cmake_build_flag}}`.
5. Do **not** modify fixed substrate (`core` / `state` / `maintenance`).
6. Do **not** copy another engine's tree wholesale — map native APIs of **this** engine onto the substrate.

## Store-mode patterns

{{optional_examples_section}}

Build with the engine CMake flag when applicable; gates run after this stage.
