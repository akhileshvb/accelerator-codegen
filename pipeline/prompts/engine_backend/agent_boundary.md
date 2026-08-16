# Engine backend agent boundary

## May create/update

- `include|src/.../runtime/{engine}/` — native scan adapter
- `include|src/.../stateful/{engine}/` — AuxBackend, StateStore, mutation/txn bridges
- CMake collectors / `ACCELERATOR_CODEGEN_BUILD_{ENGINE}` flags
- Extension/UDF registration glue for this engine

## Must not modify

- `stateful/core|state|maintenance` (fixed substrate)
- Per-query `src/generated/{benchmark}/q{N}/**` except registration appends needed for smoke
- Unrelated engines' trees unless this workload targets that engine for from-scratch regen

## Idempotence

If target files already exist and `allow_update_existing` is true: prefer **update/scaffold** over full rewrite.
