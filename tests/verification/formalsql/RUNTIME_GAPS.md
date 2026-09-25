# Runtime gaps (FormalSQL stack Phases 4–5) — Q1

## Phase 4 — C++

| Claim | Status |
|-------|--------|
| LocalWriteSafe | **IMPLEMENTED** — dirty mark + Execute fallback + clear on commit/abort |
| PRE_COMMIT invalidate | **IMPLEMENTED** — Invalidate before heap visibility |
| TokenForSnapshot | **IMPROVED** — `ForcePinActiveSnapshot` at `Runtime::Execute` first line; `PinTokenForActiveSnapshot` / `EnsureSnapshotPin` freeze `(S_encoding, epoch)` on first see of S; `CurrentVisibility` pins via callback or `EnsureSnapshotPin`; `BumpAuxMutationEpoch` at **OnPreCommit** (before Invalidate) **and** OnCommit (before TryPublish). Documented: PG statement snapshot S is established before set-returning UDF body; residual race is only concurrent Commit bump *before* first pin — filtered by pin-epoch vs publish-epoch (`TokenForSnapshot_stable_under_concurrent_publish`) |
| ReadHit / linearization | **IMPROVED** — `PostgresStateStore::ReadHitPartitions` + Q1 `Execute` uses it (complete cut or fallback; no silent omission) |
| ConsistentObservation | **IMPROVED** — required partition set must all be present |
| InvalidateBreaksStaleCAS | **IMPLEMENTED** |

## Phase 5 — Dafny substrate

| Claim | Status |
|-------|--------|
| TokenForSnapshot / SnapshotHitSafe | Shape + `MakeToken_establishes_TokenForSnapshot` + `TokenForSnapshot_stable_under_concurrent_publish` + `PinEpoch_frozen_across_bump` |
| PairInv → RuntimeAuxCorrect | `PairInv_Hit_implies_RuntimeAuxCorrect` (no free `payload==fold` requires) |
| RuntimeAuxCorrect_Q1 | **PROVED** — `tests/verification/q1/dafny/Q1RuntimeAuxCorrect.dfy`: `RuntimeAuxCorrect_Q1_from_PairInv` derives `Execute_Q1 = AuxAnswerTwin(VisibleDB, θ)` from `PairInv_Q1` + `SnapshotHitSafe_Q1` + `ReadHit_Q1` + `LocalWriteSafe` (VisibleDB collapses when clean). Wired into `Q1BackendProofs.dfy` |

C++ remains **RepPhys + finite extract-diff**, not a proved edge from `RuntimeAuxCorrect`.
