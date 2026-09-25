# Q12 verification + Q1-reuse measurement

**Research question:** Can Q12 reuse meaningful Q1 proof infrastructure, with
most new work limited to join semantics and Q12-specific state/contribution?

**Answer:** **Yes, partially.** Shared ordering/list/snapshot/maintenance
*recipes* transferred. The generated accelerator is **join-aware** (not Q1 PGA),
so the invariant, contrib, and order-side maintenance are new. Exec path is
axiom-free. Order `ApplyDelta` is `ReclassifyOrder` (reverse-index model) with
`AdjustDaysForOrder ≡ FoldLines` **proved** (no maint axioms).
`NORMALIZE_SOUND_Q12` green under named residual `Q12_FS_eq_EvalSpec`.

Do not start another query after this report. See also
[`ASSURANCE_Q1_STRENGTH_REPORT.md`](ASSURANCE_Q1_STRENGTH_REPORT.md).

---

## 1. Architecture preserved

```text
Cat(Q12) --Logos--> FormalSQL qhat
                 -> normalize = Some(S_Q12_catalog)     [PROVED]
S_Q12 / EvalResultSpec^M                               [Dafny Spec]
BuildFold Inv_Q12                                      [NEW]
Inv ⇒ ExecSeq == EvalResultSpec                        [PROVED]
MaintenancePreservesInv (ReclassifyOrder + line Δ)   [PROVED, no axioms]
Hit ⇒ Inv ⇒ Exec == EvalResultSpec                     [PROVED]
NormalizeSound_Q12 under Q12_FS_eq_EvalSpec            [PROVED shape]
```

---

## 2. Generated C++ → invariant

| Aspect | Q12 implementation |
|---|---|
| State | `order_priorities` + `day_partitions` + `order_line_index` (v2) |
| Partition | receipt day (+ separate priorities image) |
| Group key | `l_shipmode` |
| Contribution | join lookup priority → ±1 high or low |
| Finalize | merge days in `[start,end)`, keep modes ∈ {m1,m2}, sort |
| Maintenance | lineitem ApplyDelta; order ApplyDelta via reverse-index reclassify |

```text
Inv_Q12(A, Orders, Lines)  ⇔  A = BuildFold(Orders, Lines)
```

---

## 3. Reused unchanged

| Component | Role |
|---|---|
| `patterns/dafny/SortedUniqueMap.dfy` | imported; ≤2-mode order proved directly |
| `patterns/PartitionedGroupAggregate.md` | obligation checklist |
| `substrate/dafny/SnapshotPinning.dfy` | `TokenForSnapshot` reused |
| Maintenance reachability recipe | `Maint*` → `MaintenancePreservesInv` |
| Logos lower pipeline | `logos_lower_catalog.sh q12` |

---

## 4. Generalized because Q12 required it

| Change | Why |
|---|---|
| `logos_lower_catalog.sh` accepts `q12` | second FormalSQL lower |
| `NormalizeQueryQ12.v` | CrossJoin TFGOP peel |
| `QueryResultSpecQ12.v` / `Q12EvalSpec.v` | shipmode + CASE count shape |
| Fold/multiset commutation lemmas | reusable `GetDay` / `LinesCellContrib` family |

---

## 5. Assumptions removed this stop

| Residual | Resolution |
|---|---|
| `AdjustDaysForOrder ≡ FoldLines` (3 assumes) | **PROVED** (`FoldLines_insert/removePriority`, `AdjustDays_cancel`) |
| Order ApplyDelta unmodelled | `ReclassifyOrder` + Inv proofs |
| NormalizeSound absent | `NormalizeSound_Q12` + named `Q12_FS_eq_EvalSpec` |

---

## 6. Remaining TCB (Q1-analogous)

```text
Logos lowering
FormalSQL-as-reference-semantics
Q12_FS_eq_EvalSpec
SpecExportCorrect
Corresponds_Q12(C++, Model)   // unit/differential support ≠ proof
toolchain / SPI cuts
```

No AVG TCB. No maintenance axioms.

---

## 7. Metrics

```text
new Rocq:     normalize + NormalizeSound_Q12 + Q12EvalSpec
new Dafny:    AdjustDays≡FoldLines family (~15 lemmas), ReclassifyOrder Inv
shared:       SnapshotPinning; PGA recipe; SortedUniqueMap
open named:   Q12_FS_eq_EvalSpec (wider than Q1_GroupBagsPerm)
```

### Effort vs Q1

| | Q1 | Q12 |
|---|---|---|
| FormalSQL AuxCorrect | large | skipped (ResultSpec path) |
| ExecCorrect | ≈ + AVG TCB | **exact, axiom-free** |
| Maintenance | full | line + order proved, no axioms |
| NormalizeSound residual | `Q1_GroupBagsPerm` | `Q12_FS_eq_EvalSpec` |
| AVG / numeric embed TCB | yes | **N/A** |

---

## 8. Commands

```bash
scripts/run_verification_q12.sh
```

Q12 stopped.
