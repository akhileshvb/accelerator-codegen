# Engine backend agent boundary

## May create/update

- `include|src/.../runtime/{engine}/` — native scan adapter
- `include|src/.../stateful/{engine}/` — AuxBackend, StateStore, mutation/txn bridges
- Optional `src/generated/{engine}/` — extension / UDF host glue for this engine only
- CMake collectors / `ACCELERATOR_CODEGEN_BUILD_{ENGINE}` flags in `cmake/GeneratedSources.cmake` / `CMakeLists.txt`
- Artifacts under `pipeline/artifacts/engines/{engine}/`

`{engine}` is the workload `engine_name` (sanitized identifier).

## Must not modify

- `stateful/core|state|maintenance` (fixed substrate)
- `runtime/native_scan.hpp` (shared scan IR contract)
- Per-query `src/generated/{benchmark}/q{N}/**` except registration appends needed for smoke
- Unrelated engines' trees (other `runtime|stateful|{other_engine}/` directories)

## Idempotence

If target files already exist and `allow_update_existing` is true: prefer **update/scaffold** over full rewrite.

## Independence

Success must not depend on any other engine adapter being present. Sibling trees, when they exist, are optional few-shot references only.
