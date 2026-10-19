# Engine backend implementation layers

A backend run generates **slot implementations** of the runtime skeleton
(`substrate/aux/runtime/engine_slots.hpp`, S1-S8; `shared/engine_interfaces.md`,
"Backend contract"). The runtime flow is substrate and frozen: you never
implement commit order, gap handling, catch-up, build scheduling, versions,
reader/build locks or a thread pool.

Each layer ships **code + a value-checked harness** and is steered by MCP
`request_gate` (`engine_build` then that layer's gate) until both pass.
Later layers may use earlier objects; they must not skip them. A later layer
may **minimally** patch a prior layer when a real bug or design flaw blocks
this layer's contract — keep that prior harness green (add the missing assert
if it was untested). Do not rewrite prior layers for style or convenience.

The only query module allowed in this pipeline is a **toy** `AuxQueryModule`
owned by `backends/{engine}/tests/` (reference: `SumModule` in
`eval/tests/test_aux_runtime.cpp`) — never generated TPC-H / SEC accelerators.
From the `runtime` layer on, the harness runs it on a real `RuntimeHost` with
this backend's `EngineSlots`.

Do **not** regenerate `substrate/aux/**`.

| Order | `id` | Gate | Slots | What to implement | Harness must prove |
|------|------|------|-------|-------------------|--------------------|
| 1 | `capture` | `engine_capture` | S1 | The engine's real capture hook delivering one `ColumnarBatch` per statement and table to `RuntimeHost::OnCapture`, for any connection. | INSERT / UPDATE / DELETE from **two connections**: table id, kind, row count, old/new column values (NULLs, wide decimals); one batch per multi-row statement; the batch carries exactly `CaptureColumns(table)` and widens when a second accelerator registers; rolled-back transactions and subtransactions never reach a commit; a non-row change (TRUNCATE) invalidates; DDL on a tracked table never leaves a stale answer. |
| 2 | `epochs` | `engine_epochs` | S2, S3, S4 | Commit epoch (`PendingEpochLowerBound` + the epoch passed to `RuntimeHost::Commit`), reader epoch, version floor (`OldestReaderEpoch`), wired to the engine's real commit / abort lifecycle (`PreCommit` / `Commit` / `Abort`). | Epochs strictly increase per accelerator and in visibility order under concurrent writers (a snapshot that sees commit `n` sees all earlier ones; two accelerators, no deadlock); reader epoch exact: `epoch(c) <= E` iff the snapshot sees `c`, including a snapshot taken between pre-commit and visibility; floor <= every open reader's epoch and advances when they close; an abort after pre-commit or a closed connection releases its place. |
| 3 | `scan` | `engine_scan` | S5, S8 | Build snapshot with epoch `b`, the registration barrier, and engine-local typed chunked scans inside the snapshot (`RunBuild`). | A commit is in the snapshot iff its epoch <= `b`; a writer commits **while** the snapshot is open and the scan does not see it; the barrier waits for a writer that began before registration and returns false on timeout; seeded rows (>=3, one NULL): subset projection, a filter, exact cells, >=2 chunks; a partitioned parallel scan (`ScanPartitioned`, `native_scan_contract.md`) returns the same rows as a serial one, in table order per partition, within the build thread cap. |
| 4 | `runtime` | `engine_runtime` | S6 | The host (`EngineSlots` + the placement): the toy module on a real `RuntimeHost` over real writes and reads. | Seed -> background build -> read matches golden; DML + commit -> rematch; an invalidated state rebuilds **while writes continue** and becomes synced without a quiet window; a reader older than the build or a collapsed range answers `Fallback` / its own epoch, never newer; shipped placement: a dropped commit becomes a gap and a rebuild, a killed worker restarts and hits again; a read inside a writing transaction equals SQL; peak pool threads <= the budget; phase caps from engine settings / env observed; `TableRows` -> `DataScaleRows`; cost model resolved by engine and scale. |
| 5 | `host` | `engine_host` | S7 | SQL-visible entry point(s) calling `AcceleratorRuntime::Read` and the registries; packaging (extension / plugin). Skip when the workload does not require an in-process host (embedded engines register entry points in the `runtime` layer). | Load the host, call the entry through SQL: same golden as `runtime`, argument decoding incl. NULL, the `Fallback` path through SQL, no query named in the backend. |

Across the layers every backend also meets the requirements of `layer_contracts.md`
("Requirements every backend meets": own writes, writes judged at commit, lock
discipline, DDL events, abort and crash paths, thread knobs); the slot gates
`slot_own_writes` and `slot_ddl_events` check two of them on the finished backend.

Validation (after all layers + architecture audit) authors unseen
`backends/{engine}/tests/test_{engine}_adversarial.cpp`, then runs
`engine_adversarial` and the backend-generic `slot_*` gates
(`pipeline/gates/README_runtime_gates.md`).

## Invariants

- Slots only: no runtime flow in the backend (`acceptance_checklist.md`, `slots_only`).
- No portable cross-engine chunk IR.
- No reading/copying another engine's `backends/{other}/` tree.
- Fail closed: if the engine SDK / host cannot start, the harness exits non-zero.
