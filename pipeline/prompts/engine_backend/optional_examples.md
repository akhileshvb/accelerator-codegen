# Optional example backends (few-shot)

These are **illustrative** shapes. The pipeline must succeed if these directories are empty.

## Typed in-process (DuckDB-style)

- `*AuxBackend` implements `AuxEngineServices` + engine mutation/txn hooks
- `*StateStore` wraps `PartitionedStateStore` with typed `StateImage` payloads
- Scan: bind native table + filtered consumer → `DataChunk`

## Serialized boundary (Postgres-style)

- Same module contract; encode/decode only at the process/SHM boundary via `byte_stream.hpp`
- Mutation capture from triggers / transition tables → `MutationBatch`
- Txn callbacks forward to coordinator PreCommit/Commit/Abort

When citing examples, prefer short excerpts over copying entire files.
