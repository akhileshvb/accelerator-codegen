# Runtime (Postgres) — status for Q1 and Q21

The runtime correctness argument is the cross-backend sync protocol
(`src/stateful/postgres/aux_sync.cpp`, `src/generated/postgres/runtime.cpp`),
proved once in [`../substrate/dafny/AuxSyncProtocol.dfy`](../substrate/dafny/AuxSyncProtocol.dfy)
and instantiated per query:

| Query | Runtime theorem | Store invariant steps |
|-------|-----------------|-----------------------|
| Q1  | [`../q1/dafny/Q1Runtime.dfy`](../q1/dafny/Q1Runtime.dfy) `Hit_correct_Q1`, `Fallback_correct_Q1` | `Build_step_Q1`, `Commit_step_Q1` |
| Q21 | [`../q21/dafny/Q21Runtime.dfy`](../q21/dafny/Q21Runtime.dfy) `Runtime_hit_correct`, `Runtime_fallback_correct` | `Build_step`, `Commit_step` |

Live evidence: `../q21/runtime/` (multi-backend / savepoint / dirty-build /
TRUNCATE / collation repros and concurrent stress).  The superseded
epoch/snapshot-pin models (SnapshotPinning, ReadHandle, VersionedPairing,
Q1RuntimeAuxCorrect) were removed; they did not describe the multi-backend
runtime.  Trusted runtime facts: `pg_mvcc`, `pg_callbacks`, `pg_atomics`,
`runtime_capture`, `runtime_scan` (see `../q1/RESIDUALS.txt`,
`../q21/RESIDUALS.txt`).
