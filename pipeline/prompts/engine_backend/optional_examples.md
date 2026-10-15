# Store-mode patterns (engine-agnostic)

These describe **how** an adapter can hold aux state. They are not requirements to clone any particular existing engine.

## `typed_in_process`

- `*AuxBackend` implements `AuxEngineServices` and forwards engine mutation/txn hooks to the coordinator
- `*StateStore` wraps `VersionedAuxStore` with typed `StateImage` payloads
- `TryPublish` uses `TryPublishPartition` (CAS on `expected_version`); `Read` /
  `CheckValidity` honor `VisibilityToken` vs `valid_through_mutation_epoch`
- Keys come from published `StateImage`s — no `RegisterQueryKey`
- No serialize/deserialize on the hot path
- Scan: bind native table + filtered consumer → **this engine’s** typed
  callbacks / slots / batches (not a portable chunk IR)

## `serialized_boundary`

- Same module contracts (`AuxEngineServices` / `AuxStateStore` / `MutationBatch`)
- Encode/decode only at a process or shared-memory boundary (`byte_stream.hpp`)
- Mutation capture from native engine hooks → `MutationBatch`
- Txn callbacks forward to coordinator PreCommit / Commit / Abort

Choose from the workload `store_mode_candidates`. Prefer the mode that matches this engine's process model; document the choice in `backend_analysis.json`.
