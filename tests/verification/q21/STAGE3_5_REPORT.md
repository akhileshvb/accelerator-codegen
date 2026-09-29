# Q21 Stages 3–5: accelerator state, maintenance, execution, runtime

**Classification: VERIFIED** (2026-09-29; all 40 gates pass, including the
Q1 regressions and the live Postgres runtime gates; no BLOCKING residuals).

What this means precisely:
* The accelerator algorithm and the multi-backend runtime protocol are proved
  in Dafny at the model level (Stages 3–5).
* The SQL side is proved in Rocq (Stage 2).
* Model ↔ C++ correspondence is structural plus tested (extract-diff, live
  repros and stress, mutants), not proved.
* Every trusted edge is listed in `RESIDUALS.txt`. The C-collation domain
  for text ordering is enforced by the runtime (§4).

The shared runtime bugs E1–E4 were reproduced live, fixed, and proved sound
(§5). The `Eval.v` interpreter pin was re-pinned after review, with the user's
approval (`formalsql/resultspec/interpreter_source_relnode.pin`).

Gate: [`run_q21_verification.sh`](run_q21_verification.sh). Residuals:
[`RESIDUALS.txt`](RESIDUALS.txt).

## 0. C++ changes made before Stage 3

These are in the generated postgres, duckdb and mysql `q21/*/aux/module.cpp`,
plus the three `finalize.cpp` files. The repro and regression harness is
[`cpp/`](cpp/).

| # | Bug | Fix |
|---|-----|-----|
| 1 | Maintenance on an order that Build pruned used an empty summary and defaulted its status to 'F' | Deleted in the batch → skip. Inserted in the batch (RF1) → start from empty. Otherwise → RebuildAll. |
| 2 | Nation rename or delete left a stale, Valid name-partition fast-path image | Nation mutations → MarkRebuild |
| 3 | GROUP BY s_name was emitted per suppkey | Rows are merged by name before sorting (MergeByNameAndSort) |
| 4 | RF1: delta suppliers of an unsummarized order were never pre-loaded, so republishing wiped the nation's counts | Delta suppliers' nations are pre-loaded regardless of summary presence |
| 5 | Duplicate nation names: ApplyDelta republished the name image from a single nation | Name images are republished only when the name has exactly one nation |

## 1. Dafny development (`dafny/`)

| File | Content |
|------|---------|
| `Q21Types` | Typed DB `TDB` and `ToRel(T)` into the RelNode DB; `Q21Params`; PK and RI predicates (`KeysOk`) |
| `Q21SqlCore`, `Q21SqlAgg` | Semantic core: the Dafny RelNode interpreter on `LogicalSpec_Q21_rel` |
| `Q21Count`, `Q21SetSum`, `Q21Contrib` | COUNT decomposition: per supplier, then per order |
| `Q21Answer`, `Q21Sorted`, `Q21Emit` | The answer as a set; sorted uniqueness; TextCLeb ↔ StrLt bridge |
| `Q21ModuleTwin` | Compiled structural twin of the postgres `module.cpp`, with a source map |
| `Q21Inv` | `StoreInv` (Inv_Q21) and `Execute_correct` |
| `Q21Refine`, `Q21Build` | Refinement of contribution and counting loops; Build establishes Inv; exact fallback |
| `Q21Delta`, `Q21MaintNw`, `Q21Maint` | Delta DB semantics; every maintenance arm |
| `Q21Perm` | Invariance of Inv and of the SQL result under row order |
| `Q21Runtime` | Multi-backend runtime protocol model: build / own-commit / hit soundness, hit ⇒ SQL |

There are no `assume`, `{:axiom}` or `{:verify false}` annotations (checked by the gate). The only opaque
functions are `SummaryOf` and `SumAt`, both revealed where needed.

## 2. Inv_Q21 (`Q21Inv.StoreInv(snap, T)`)

For every published Q21 partition in a fully valid store:

* **p0 SupplierMetadata:** `supplier_names = SupNameOf(T)` and `supplier_nations = SupNatOf(T)`.
* **p1 OrderSummaries (`OrdersOk`):**
  * A summarized order `ok` has summary `OrderSummary(StatusOf(T,ok), SummaryOf(T.li,ok))`, i.e. the exact per-supplier (total, late) counts.
  * An unsummarized order contributes nothing: `Cv(T,ok,·) = 0`.
* **p2 NationNameIndex (`IdxOk`):** name → the distinct nation keys carrying that name.
* **1000+nk (nation images):**
  * `nation_numwait[nk] = NWmap(T,nk)`, where `NWmap(T,nk) = {sk ↦ NW(T,sk) | nation(sk)=nk, NW≠0}`.
  * `output_rows` answers Q21 for `{nk}`.
* **2000000+h (name images):**
  * The image is at `NationNamePartition(its name)`.
  * `NationsNamed(T,name) = {nationkey}`.
  * `output_rows` answers Q21 for that nation.
* **Absent nation images:** an absent `NationPartition(nk)` means `NWmap(T,nk) = ∅`.

Semantic bridge (proved, not assumed):
* `PerOrder`: the number of qualifying lineitems of (order, supplier) equals `ContribMap(status, SummaryOf)`, the C++ "exactly one late supplier among more than one" rule.
* `NW_by_orders`: `NW(T,sk) = Σ_orders Cv`.
* `SpineNameCount`: the SQL `COUNT(*)` per `s_name` equals the sum of `NW` over that name's suppliers in the selected nations.

Multiplicity is preserved exactly.

## 3. Theorems

| Stage | Theorem | Statement (premises) |
|-------|---------|----------------------|
| 4 core | `Q21SqlAgg.Q21_EvalSpec_Answer` | `Eval_Spec(LogicalSpec_Q21_rel, nation_name↦nn, ToRel(T)) = SomeRows(PairRows(AnswerPairs(T,nn)))`. **No premises.** |
| 4 core | `Q21Answer.SQLRows_sorted_answer` | Under `KeysOk(T)`, the SQL rows are the unique strictly (numwait DESC, s_name ASC)-sorted enumeration of `AnsSet(T, NationsNamed(T,nn))`. |
| 3.4 | `Q21Build.Build_establishes_Inv` | `KeysOk ∧ DomOk ⇒ StoreInv(Install(BuildImages(T)), T)`, for every hash-iteration order. |
| 4 | `Q21Build.ExecuteFallback_correct` | `ExecuteFallback(T, nn) = SQLRows(T, nn)` |
| 3.5 | `Q21Maint.Maint_ApplyDelta` | `StoreInv(snap,T) ∧ TxnOk(T,T',b) ⇒` the ApplyDelta outcome publishes images satisfying `StoreInv(·,T')`. `T'` is `T` with the transaction's valid lineitem and order deltas applied. |
| 3.5 | `Q21Maint.Maint_other_arms` | Every other outcome invalidates all partitions, or is NoOp with no Q21 deltas. |
| 3.5 | `Q21Maint.Accumulate_counts` | AccumulateMutation never produces supplier or nation deltas (it rebuilds instead). |
| 4 | `Q21Inv.Execute_correct` | `StoreInv(snap,T) ∧ KeysOk ∧ DomOk ⇒` every emitted hit (fast path, single-nation slow path, multi-nation slow path, empty) equals `SQLRows(T,nn)`. |
| 4.2 | `Q21Sorted.StrictSorted_unique` + `TextCLeb_StrLt` | Ordering is exact. On distinct names, `std::sort`/`BetterOutputRow` and the RelNode sort (`TextCLeb`) agree. |
| 3.6 | `Q21Refine.Contrib_refines`, `Q21Emit.MergeByName_sorted`, `Q21Build.SumF_enum` | Twin loops equal the Aux math for **every** enumeration order. |
| — | `Q21Perm.StoreInv_perm`, `SQLRows_perm` | Inv and the SQL answer depend on each table only as a multiset. |
| 5 | `Q21Runtime.Hit_sound`, `Build_sound`, `OwnCommit_sound` | The sync protocol: a passing hit check ⇒ the statement sees exactly the writers the store reflects (§5.3). |
| 5 | `Q21Runtime.Runtime_hit_correct`, `Runtime_fallback_correct` | `SInv ∧ HitCheck ⇒ RuntimeExecute = SQLRows(Tv) = rows of Eval_Spec(LogicalSpec_Q21_rel, ToRel(Tv))`; fallback likewise. |
| 5 | `Build_step`, `Commit_step` (`Install_inv`, `Commit_inv`) | Rebuild and own commit preserve `StoreInv(store, dbof(R))`. |

**Twin ↔ Aux.** The Twin is proved directly against the Aux specification:
* The Aux level is the math layer: `SummaryOf`, `Cv`, `NW`, `NWmap`, `AnsSet`, `StoreInv`.
* The representation relation is the per-image projection:
  * `FindOrder` flattens overlay over base;
  * `NwOf` reads `nation_numwait`;
  * `output_rows` is read directly.
* This projection is representation-only; the DB meaning lives in `StoreInv`.
* There is no separate Aux state machine and no refinement residual.

## 4. Verified-domain premises

* `KeysOk(T)`:
  * PKs `s_suppkey`, `n_nationkey`, `o_orderkey`;
  * FK `l_orderkey → o_orderkey`. Only RF1 new-order handling needs it.
* `DomOk(T)`: nation keys lie in [0, 1998999], so partition ids are disjoint (after the Postgres `int32` truncation).
* Order status is a single non-NUL char; strings are byte strings.
* **Collation (enforced).** The reference semantics and the accelerator both order `s_name` byte-wise. PostgreSQL agrees exactly when the column's collation is C-compatible (`lc_collate_is_c`). The runtime checks this on every call and raises an ERROR otherwise; it fails closed and never returns a possibly different order (repro E5). Every TPC-H text column is now `COLLATE "C"`: the schema script covers new databases, and `scripts/postgres_tpch_c_collation.sql` migrates existing ones (`accel_tpch_test`: 29 columns). Equality predicates and `GROUP BY` need nothing extra, since text equality under any deterministic collation is byte equality.
* Counts fit in int64 (`|lineitem| < 2^63`).
* Transaction deltas are valid: deletes and updates refer to existing rows, inserts respect the PK.
* Stage 2 premises (`Preconditions_Q21`, `Q21CountSafe`) apply to the SQL side.

## 5. Stage 5 runtime: fixed and proved (protocol model)

### 5.1 The bugs (all reproduced live on Postgres 17, then fixed)

`runtime/q21_runtime_repro.py` runs against the `accel_pg_test` container. Before the fix it showed 5 mismatches:
* **E1, multi-backend:** each backend keeps a private store. Session A built its store; B committed a contributing order; A's next call hit the pre-B image (`Supp#1 1` vs SQL `2`). This also happens when B commits after being in flight during A's query.
* **E2, savepoints:** `SAVEPOINT; INSERT; ROLLBACK TO; COMMIT` still applied the rolled-back delta (`2` vs `1`). `RELEASE` then double counted (`3` vs `2`).
* **E3, build inside a dirty transaction:** `BEGIN; INSERT; accel(); ROLLBACK`. The in-transaction build kept the aborted insert in a Valid store (`2` vs `1`).
* **E4, TRUNCATE (found during the fix):** TRUNCATE fired no capture trigger, so other backends were never told.

### 5.2 The fix (shared runtime; applies to every Postgres accelerator)

| File | Change |
|------|--------|
| `src/stateful/postgres/aux_sync.cpp` (new) | Per-database named DSM segment with, per table slot, `inflight` and `seq` counters. The capture trigger does `inflight++` on the first write of a slot. COMMIT (after `ProcArrayEndTransaction`) does `seq++` then `inflight--`; ABORT, or a commit without an xid, does `inflight--` only. Also provides snapshot marks (`xmax`, `xip ∪ subxip`), `Covers`, `Sees`. PREPARE of a tracked write is refused. |
| `src/generated/postgres/runtime.cpp` | Sync marker (build snapshot mark, own maintained commits, seq per dependency slot). **Hit** requires: marker synced; store fully valid; no captured dependency write in this transaction; the statement snapshot covers the build snapshot and sees each own commit; and, read after the snapshot, `inflight==0` and `seq==marker` on every dependency slot. Otherwise the call uses `ExecuteFallback`. **Rebuild** happens only in a transaction without an xid, from a fresh snapshot pushed for the whole Build, between two counter reads. **Own commit** keeps the marker only if the PRE_COMMIT check (only this transaction in flight, seq unchanged) and the COMMIT `fetch_add` results match. `EnsureBuilt` no longer clears the dirty flag. |
| `src/stateful/postgres/postgres_xact_callbacks.cpp` | Maintenance snapshot at PRE_COMMIT is `GetLatestSnapshot()`, which is also correct under REPEATABLE READ. The COMMIT path no longer throws past the point of no return. `RegisterSubXactCallback` is wired up. |
| `src/stateful/core/aux_registry.cpp` | `OnMutationAtDepth` / `{Commit,Abort}SubtransactionAtDepth`: frames keyed by PG nest level, opened lazily. Debug `fprintf`s removed. |
| `src/generated/postgres/transition_capture.cpp`, `scripts/postgres_accel_mutation_triggers.sql` | Registers every captured statement; passes the nest depth; new `AFTER TRUNCATE` trigger. |

### 5.3 Proof (`../substrate/dafny/AuxSyncProtocol.dfy`, query-generic and shared with Q1; instantiated by `dafny/Q21Runtime.dfy`)

The model contains every writer in every backend, each with per-slot register / visible / count / release times. `History` states only the orderings that the code and PG callback order guarantee. Snapshots are modelled by content; `SnapAt` is PG's MVCC contract (TCB `pg_mvcc`). Counter reads are per slot: inflight first, seq later.

* `Build_sound`: counter read, then fresh snapshot, then counter read (inflight 0, seq unchanged). This yields a marker for `R = dependency writers visible to the build snapshot`.
* `OwnCommit_sound`: the PRE_COMMIT and COMMIT checks make the maintenance base exactly `R`, and the marker describes `R ∪ {A}`.
* `Hit_sound`: a passing hit check means the dependency writers visible to the statement snapshot are exactly `R`.
* `Build_step`, `Commit_step`: `StoreInv(store, dbof(R))` is preserved. They reuse `Build_establishes_Inv` and `Maint_ApplyDelta` through `CaptureOk` (TCB `runtime_capture`).
* `Runtime_hit_correct`, `Runtime_fallback_correct`: the returned rows are `SQLRows(Tv)`, which equals the rows of `Eval_Spec(LogicalSpec_Q21_rel, Tv)`. `Tv` is the statement's database.

The premises E1–E3 are gone. The remaining environment facts are explicit TCB items (`RESIDUALS.txt`: `pg_mvcc`, `pg_callbacks`, `pg_atomics`, `runtime_capture`, `cpp_runtime_model`).

### 5.4 Live evidence (finite)

* `q21_runtime_repro.py`: E1, E1b, E2, E3, E4, E5 (collation guard), with hit or fallback asserted from the runtime's DEBUG1 trace. **0 mismatches.**
* `q21_runtime_stress.py`: 4–8 concurrent sessions run random READ COMMITTED / REPEATABLE READ transactions (writes, TRUNCATE, savepoints, rollbacks, queries inside write transactions). Accel and SQL are compared in one statement, so both use one snapshot. On the final build: 60 s × 4 gave 55,460 queries (7,885 hits); 120 s × 8 gave 162,445 queries (2,207 hits). **0 mismatches.**
* Mutants: dropping the counter check → 441 mismatches in 20 s. Dropping the snapshot-coverage check → 46 mismatches in 30 s. Both checks are necessary.
* Regressions: the existing Postgres TPC-H differentials for all 22 queries (read, write+maintenance, multi-connection, cross-accelerator). See §10.

**Cost.** A backend's store resyncs (full Build) after any other backend commits to a dependency table, and it falls back while any such writer is in flight. With per-process stores that is the price of correctness. A shared-memory store would avoid it.

## 6. C++ ↔ Twin correspondence (finite, not universal)

* **Structural:** each Twin function maps to its C++ location (`SOURCE MAP` header of `Q21ModuleTwin.dfy`).
* **Extract-diff:** `cpp/q21_twin_trace.cpp` runs the real postgres module on seeded scenarios. `cpp/q21_twin_replay.py` replays them on the compiled Dafny twin (Dafny → Python). It compares maintenance action, invalidate-all, published partition ids, store validity and every emitted row.
  * Result: 1000 scenarios × 40 transactions, 123,000 queries, 6,433 ApplyDelta commits, **0 mismatches**.
  * Mutation check: reverting fix #4 in a copy yields 452 mismatches.
* **SQL differential:** `cpp/run_q21_aux_differential.sh` covers all three engines, 3000 × 40 per engine, 0 mismatches.

This is finite testing. C++ ↔ Twin remains the TCB item `cpp_model`.

## 7. End-to-end claim (composed; not a single machine-checked C++→SQL theorem)

For every `nn` and every statement snapshot `S` of any backend, under the §4 premises and the Stage 2 premises:

    Obs(RuntimeExec_Q21(S, nn)) = rows of Eval_Spec(LogicalSpec_Q21_rel, nn, DB(S))   [Dafny, Q21Runtime]
                                = the unique SqlSuccess outcome of qhat_Q21 nn on DB(S)  [Rocq, NormalizeSound_Q21_all_outcomes]

This holds for hits and fallbacks alike.

The trusted edges are:
* `spec_export_B`: the Rocq↔Dafny interpreter correspondence;
* `db_embedding`;
* `cpp_model` and `cpp_runtime_model`: extract-diff, live repros and stress;
* `cpp_substrate`;
* `runtime_capture`;
* `pg_mvcc`, `pg_callbacks`, `pg_atomics`;
* FormalSQL / Logos.

## 8. Print Assumptions / axioms

* **Rocq (Stage 2):** unchanged. `NormalizeSound_Q21_all_outcomes` uses the five stdlib axioms (see `formalsql/q21/allowed_assumptions.txt`).
* **Dafny:** no `assume`, axioms or `{:verify false}`. The gate greps for them, and every file verifies with 0 errors and 0 warnings.

## 9. Proof-library harvest

Deferred: the task allows harvesting only after all Q21 stages are green. The reusable candidates for later harvest:
* `Q21SetSum` (set sums);
* `SumF_perm` / `SumF_enum`;
* `StrictSorted_unique` + `TextCLeb_StrLt`;
* the interpreter-characterization pattern for joins, semi/anti joins, a group-count fold and sort (`Q21SqlCore` / `Q21SqlAgg`);
* the opaque-summary technique.

## 10. Regression results on the final build

The repo's existing Postgres TPC-H suites ran on the final runtime (`accel_tpch_test`):

| Suite | Result |
|-------|--------|
| `run_postgres_tpch_diff_all22.sh`, read | 22/22 pass (88 argument variants) |
| `run_postgres_tpch_diff_all22.sh`, write_maint | 22/22 pass |
| `run_postgres_tpch_conn_all22.sh`, multi-connection + cross-accelerator | 44/44 pass |

Notes:
* **Local link stub.** The C++ test binaries were linked with a scratch stub for four PG server symbols. The user's uncommitted `cmake/GeneratedSources.cmake` edit puts `snapshot_token.cpp` into `libaccelerator_codegen.so`, so client-side test executables no longer link. The stub is not in the repo.
* **Possible leftover data change.** An earlier write_maint run on a pre-final build was stopped during Q6. Its restore step may not have run, so `accel_tpch_test` may carry one modified row. The differentials compare accelerator and SQL on the same data, so results are unaffected.

