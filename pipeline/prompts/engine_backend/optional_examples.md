# Store-mode patterns (engine-agnostic)

These describe **how** an adapter can hold aux state. They are not requirements to clone any particular existing engine.

## `typed_in_process`

- `*AuxBackend` implements `AuxEngineServices` and forwards engine mutation/txn hooks to the coordinator
- `*StateStore` derives from `VersionedAuxStore` (sets its `Options`) with
  typed `StateImage` payloads (`StateImage::Make` / `As<T>`)
- Versions, `expected_version` checks and `VisibilityToken` selection come
  from `VersionedAuxStore`; keys come from published `StateImage`s
- No serialize/deserialize on the hot path
- Scan: bind native table + filtered consumer → **this engine’s** typed
  callbacks / slots / batches (not a portable chunk IR)

## `serialized_boundary`

- Same module contracts (`AuxEngineServices` / `AuxStateStore` / `MutationBatch`)
- Encode/decode only at a process or shared-memory boundary (`byte_stream.hpp`);
  each process keeps its own `VersionedAuxStore` and refreshes it by rebuild +
  `Install`
- Mutation capture from native engine hooks → `MutationBatch`
- Txn callbacks forward to coordinator PreCommit / Commit / Abort

Choose from the workload `store_mode_candidates`. Prefer the mode that matches this engine's process model; document the choice in `backend_analysis.json`.
