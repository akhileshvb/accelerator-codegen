# Aux runtime skeleton

The engine-neutral runtime every backend hosts. The substrate owns the flow:
capturing writes per transaction, ordering commits per accelerator, maintaining
state, background builds with catch-up, and reads at a snapshot's epoch. A
backend supplies only the **slots** in `engine_slots.hpp`. Per-query modules are
unchanged: they still implement `core::AuxQueryModule`.

Design note: "Postgres Shared State Redesign" (contracts C1–C12).

## Pieces

| Header | Contract | What it is |
|---|---|---|
| `columnar_batch.hpp` | C5 | `ColumnarBatch`, the captured change of one statement on one table: typed columns, the union of the columns any accelerator watches, shared by reference. `ToMutationBatch` gives a legacy module its projected `core::MutationBatch`. |
| `engine_slots.hpp` | C2 | The slots S1–S8 a backend implements. |
| `thread_budget.hpp` | C12 | One thread budget per backend; the shared pool every parallel step draws from. |
| `sequenced_applier.hpp` | C3 | Per accelerator: orders committed changes by sequence number, detects gaps, catches a fresh build up. |
| `accelerator_runtime.hpp` | C1, C4, C7, C11 | Per accelerator: module + store + applier + background build + reads. |
| `runtime_host.hpp` | C1 | Per database (or per Postgres shard worker): the accelerators, writer transactions, the pool. |

## Numbers attached to a commit

Every commit of an accelerator carries two numbers:

- **sequence** (`Seq`): dense per accelerator, 1, 2, 3, ... in the order the
  accelerator's commits become visible. Assigned under the accelerator's commit
  order (S2). Used for ordering and gap detection.
- **epoch** (`Epoch`): the engine's visibility clock, the scale reader epochs
  (S3) and build epochs (S5) use. A version published at epoch `e` is visible
  to a reader at epoch `E` iff `e <= E`. Strictly increasing per accelerator.
  - DuckDB: commit id + 1; reader epoch = transaction start time.
  - Postgres: epoch = sequence = `n_A`; reader epoch `k_A`.
  - MySQL: epoch = sequence; readers read the newest.

A build snapshot has an epoch `b` with the guarantee: a commit is in the
snapshot iff its epoch `<= b` (S5's contract).

## Write path

1. S1 delivers each statement's change as a `ColumnarBatch` for a writer
   transaction (`RuntimeHost::OnCapture(txn, depth, batch)`). The host keeps the
   batches per transaction (and per subtransaction depth). Nothing is
   interpreted yet.
2. Pre-commit (`RuntimeHost::PreCommit(txn)`): for each accelerator whose
   tables the transaction wrote, in accelerator-id order, the commit takes the
   accelerator's place in commit order (S2) and receives its sequence number.
   - In-process placement: the accelerator's maintenance is staged here, on the
     writer's thread, from the transaction's batches (Interpret, Accumulate,
     StageMaintenance on a fresh txn buffer), and `BeginCommit` marks it
     pending.
3. Commit (`RuntimeHost::Commit(txn, epoch_of)`): the engine has committed;
   each accelerator publishes the staged state at the commit's epoch
   (`FinishCommit`).
   - Shipped placement (Postgres): the writer delivers `(seq, epoch, batches)`
     to the worker and waits for the apply, bounded. The worker's applier does
     steps 2-3 in sequence order.
4. Abort: staged work is dropped; the sequence number, if taken, is released
   as a no-op so the sequence stays dense (`SequencedApplier::Skip`).

A commit is never skipped or dropped. If the accelerator's state is being
rebuilt, or is invalid, the commit is queued in its applier for catch-up.

## Builds (C4)

Builds never run inside a writer's commit. A build:

1. waits out writers that started before the accelerator's capture was
   registered (S5 `WaitForPreRegistrationWriters`, first build only);
2. opens a build snapshot (S5) with epoch `b`; scans through it (S8);
3. installs the result at epoch `b`;
4. catches up: the applier drops queued commits with epoch `<= b` and applies
   the rest as one combined coordinator transaction;
5. is synced again: later commits apply one by one.

Readers whose epoch is below the newest published version keep reading their
versions throughout. Readers that need a version not built yet wait (bounded)
or fall back by cost (C11).

## Reads (C7, C11)

`AcceleratorRuntime::Read(epoch, args, sink)` resolves the version at `epoch`
inside an `AuxReadScope` and runs `module.Execute` on the calling thread. It
never takes the commit or build locks. If no version can serve `epoch`:
wait up to the wait budget for a pending commit, build or catch-up, else
return `ReadResult::Fallback` and the backend runs `ExecuteFallback`.

## Threads (C12)

`ThreadBudget` is the only source of parallelism: builds of several
accelerators, partition-parallel staging, and intra-query `Execute` take
workers from its pool. Its size is the backend's knob (DuckDB `threads`,
Postgres `accel.threads`, MySQL `accel_threads`), `ACCEL_AUX_THREADS`
overriding; the default is the CPU count of the process's affinity mask.
