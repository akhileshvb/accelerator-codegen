# Q1 → PartitionedGroupAggregate extraction report

Superseded by the fuller library refactor report:

**[`../REUSE_LIBRARY_REPORT.md`](../REUSE_LIBRARY_REPORT.md)**

(PGA recipe: [`../patterns/PartitionedGroupAggregate.md`](../patterns/PartitionedGroupAggregate.md))

Stop point: `EXEC_PROVED` + `MaintenancePreservesInv` green; no Q21.

## Verdict

Yes — for accelerators with the same **PartitionedGroupAggregate** shape, the
expensive pieces that matter next time are:

1. **GENERIC** `SortedUniqueMap.SameMapSameSortedSeq` (ordering comparison)
2. **PATTERN-GENERIC** filter/fold/compose morphisms (structure copyable; not
   parameterized in Dafny — abstract-module refinement was abandoned)
3. **Q1-thin** `key` / `accept` / `contrib` correspondence lemmas

A second PGA accelerator should **not** need a new induction architecture; it
needs morphisms + finalize + Spec correspondence. Full ordered sequence
`ExecSeq ≈ Spec` still needs one representation glue step (see Ordering).

---

## 1. Lemma classification

### `Q1ResultSpecBridge.dfy` (current)

| Lemma / predicate | Category | Notes |
|---|---|---|
| `key_Q1_eq_SpecKey` | Q1-SPECIFIC | Pack(rf,ls) ↔ Spec.RowKey |
| `accept_Q1_eq_SpecFilter` | Q1-SPECIFIC | shipdate ≤ cutoff |
| `contrib_Q1_eq_SpecContrib` | Q1-SPECIFIC / REPRESENTATION | Apply ↔ SpecApply ($f4/$f5) |
| `AuxRowsToSpec_snoc` | REPRESENTATION-SPECIFIC | Aux↔Spec row transport |
| `FilterCutoff_eq_Spec` | PATTERN-GENERIC | filter morphism (PGA Accept) |
| `FoldByGroup_eq_Spec` | PATTERN-GENERIC | group-fold morphism |
| `Compose_eq_SpecView` | PATTERN-GENERIC | filtered compose = SpecView |
| `Compose_positive_counts` | PATTERN-GENERIC | fold contrib count > 0 |
| `FinalizeOne_Approx` | Q1-SPECIFIC + REPRESENTATION | finalize + AvgObservationFaithful TCB |
| `Q1ExecCorrect` | Q1-SPECIFIC (thin) | Inv ⇒ per-group ≈_Q1 |
| `EvalResultSpec_sorted_ok` | Q1-SPECIFIC glue | wraps Spec.EvalResultSpec_sorted |
| `ExactEntries_len` / `SpecExactEntries_len` | REPRESENTATION-SPECIFIC | project OutRow → Entry |
| `ExactEntries_HasKey` / `SpecExactEntries_HasKey` | REPRESENTATION-SPECIFIC | |
| `ApproxQ1_of_strict_sorted_same_keys` | PATTERN-GENERIC glue | uses GENERIC SameMapSameSortedSeq |
| `AvgObservationFaithful` | REPRESENTATION-SPECIFIC TCB | NUMERIC→float8 observation |

### `generated/Q1ResultSpec.dfy`

| Item | Category |
|---|---|
| `Cutoff`, `SpecApply`, `Pack`/`RowKey`, `FilterCutoff`, `FoldByGroup`, `SpecView`, `EvalResultSpec` | Q1-SPECIFIC Spec export |
| `InsertSorted_sorted`, `SortByKey_sorted`, `EvalResultSpec_sorted` | PATTERN-GENERIC (sort algebra; Spec-typed duplicate of SortedUniqueMap) |

### Reused from `Q1Aux.dfy` (maintenance / Inv — not rewritten)

| Lemma | Category |
|---|---|
| `InvPreservedByInsert` / `InvPreservedByDeleteLast` / `InvPreservedByDeleteAt` | PATTERN-GENERIC (PGA cell update) |
| `FoldGetEqualsContrib` / `FoldFromContrib` / `Contrib*` | PATTERN-GENERIC fold-Inv |
| `ApplyCancel` / `MergeAssociative` / … | Q1-SPECIFIC algebra on Stats vector |
| `ComposeStep` / `FilterCutoffStep` / `FoldByGroupStep` | PATTERN-GENERIC |
| `ComposeEqualsFilteredFold` | PATTERN-GENERIC (definitional) |

### `Q1Finalize.dfy` / `Q1ModuleTwin.dfy`

Used for finalize/AVG observation and twin Exec surface — **REPRESENTATION / Q1**,
not extracted. Twin lemmas stay out of the pattern library.

### `patterns/dafny/SortedUniqueMap.dfy` — **GENERIC**

| Lemma | Role |
|---|---|
| `InsertSorted_sorted`, `SortByKey_sorted` | sort preserves Sorted |
| `Strict_drop_head`, `SortedUnique_implies_Strict`, `StrictSorted_implies_Unique` | uniqueness ↔ strict order |
| `SameKeys_nonempty_heads`, `SameMapSameSortedSeq_tails` | helpers |
| **`SameMapSameSortedSeq`** | **main reusable ordering theorem** |

### `patterns/dafny/PartitionedGroupAggregate.dfy`

Documentation only (no proved defs). Records why abstract-module PGA was
abandoned and points at SortedUniqueMap + Q1-shaped morphism lemmas.

---

## 2. Does PartitionedGroupAggregate capture Q1?

**Yes**, for the ExecCorrect PoC form:

```text
partition(r) = shipdate
key(r)       = Pack(rf, ls)
accept(p, θ) = p ≤ cutoff(θ)
contrib      = Apply / SpecApply
Inv(A,D)     ⇔ A = FoldTwin(D)   (day×key cells)
Compose      = FoldByGroup(Filter(accept))
```

`Q1ExecCorrect` does **not** need the day-partition store `A` in the statement:
under `Inv`, Compose is defined from `D`, matching
`SpecView = FoldByGroup(FilterCutoff)`. The partition dimension matters for
**maintenance** (`ApplyDelta` on `(shipdate, Pack)` cells), which already lives
as PATTERN-GENERIC Inv lemmas in `Q1Aux` and was left untouched.

---

## 3. Reusable modules added

```text
tests/verification/patterns/dafny/
  SortedUniqueMap.dfy          ← GENERIC, verified (18 obligations)
  PartitionedGroupAggregate.dfy ← notes only (no abstraction layer)
```

**Not** added (would not reduce Q1 work / caused solver friction):

- parameterized `GroupAggregate.dfy` / `FilteredCompose.dfy` abstract modules
- rewrite of `Q1Maintenance` onto generic insert/delete theorems

---

## 4. Metrics

### Before (EXEC_PROVED PoC bridge, pre-extraction simplify)

| Metric | Value |
|---|---|
| Q1ResultSpecBridge LOC | **234** |
| Bridge lemmas | **9** (`FilterCutoff`/`Fold`/`Compose` morphisms + `Q1ExecCorrect` + sorted_ok) |
| Induction/fold morphisms in bridge | **4** (`snoc`, `FilterCutoff_eq_Spec`, `FoldByGroup_eq_Spec`, `Compose_positive_counts`) |
| Generic library | none |

### After

| Metric | Value |
|---|---|
| Generic library lemmas (`SortedUniqueMap`) | **8** (core: `SameMapSameSortedSeq`) |
| Pattern notes module | docs only |
| Bridge LOC | **416** (grew: thin morphisms + ordering projection glue) |
| Remaining Q1-specific bridge lemmas | **~6** true Q1 (`key`/`accept`/`contrib`, `FinalizeOne_Approx`, `Q1ExecCorrect`, `EvalResultSpec_sorted_ok`) |
| Pattern-generic morphisms still in bridge | **4** (same induction structure; typed to Q1 Row/Stats) |
| Representation projection lemmas | **4** (`ExactEntries_*` / `SpecExactEntries_*`) |
| `MaintenancePreservesInv` | **green** (32 verified, unchanged) |
| `Q1ResultSpecBridge` | **green** (44 verified) |

Honest note: bridge LOC did **not** shrink. The win is **reusable ordering
algebra** + **named thin morphisms**, not fewer Q1 lines. Parameterized PGA
modules would have *increased* friction without cutting the morphism proofs.

### Lemmas that became “simple instantiations”

| Lemma | Now |
|---|---|
| `key_Q1_eq_SpecKey` | trivial body |
| `accept_Q1_eq_SpecFilter` | trivial body |
| `contrib_Q1_eq_SpecContrib` | trivial body |
| `ApproxQ1_of_strict_sorted_same_keys` | thin wrapper around `SameMapSameSortedSeq` |

---

## 5. Ordering status

| Claim | Status | Where |
|---|---|---|
| Per-group ≈_Q1 under Inv | **PROVED** | `Q1ExecCorrect` (`EXEC_PROVED`) |
| Spec sequence StrictSortedByGroupKey | **PROVED** | `EvalResultSpec_sorted` |
| Same unique map + StrictSorted ⇒ same seq | **PROVED GENERIC** | `SortedUniqueMap.SameMapSameSortedSeq` |
| Position-wise ≈_Q1 given strict-sorted same keys | **PROVED** | `ApproxQ1_of_strict_sorted_same_keys` |
| `Inv ⇒ ApproxQ1(ExecSeqM, EvalResultSpec)` | **PROVED** | `Q1ExecCorrectOrdered` (`ORDERING_PROVED`) |

Construction: both sides finalize ascending Pack keys via `KeysFrom(0,65536,·)` /
`AuxKeysFrom` (deterministic model of KeyStore enum + sort). No open residual.

`EXEC_PROVED` is the per-group semantic gate; `ORDERING_PROVED` is the
sequence-level gate via `Q1ExecCorrectOrdered`. No open construction residual.

---

## 6. What a second PGA accelerator must generate

```text
1. partition / key / accept / contrib / merge   (query morphisms)
2. Spec twin of Filter+Fold+View+Finalize       (export)
3. Thin eqs: key=SpecKey, accept⇔SpecFilter, contrib=SpecContrib
4. Copy pattern-shaped Filter/Fold/Compose morphisms (or share once parameterized)
5. Finalize + AvgObservationFaithful (if AVG) or exact observation
6. Reuse `KeysFrom(0, keyBound)` (or equivalent) for deterministic sorted enum;
   then `ApproxQ1_of_strict_sorted_same_keys` / `SameMapSameSortedSeq`
7. Maintenance: reuse InvPreservedByInsert/Delete pattern on (partition,key) cells
```

**Should not need:** a new proof that “same sorted unique maps are equal”, nor
a new FormalSQL↔AuxAnswerSpec handshake.

**Still per-accelerator:** contrib algebra, finalize schema, representation
encoding (Pack, day buckets), observation TCB, key-universe bound for `KeysFrom`.

---

## 7. What stayed query-specific (intentionally)

- Q1 Stats vector / $f4/$f5 scaling
- `AvgObservationFaithful`
- Pack(rf,ls) encoding
- Day-partitioned `Store` layout in Inv (maintenance)
- Filter/Fold morphism *types* (not lifted to abstract modules)

---

## Commands (gates)

```bash
tools/dafny/dafny verify tests/verification/patterns/dafny/SortedUniqueMap.dfy
tools/dafny/dafny verify tests/verification/q1/dafny/Q1ResultSpecBridge.dfy --allow-warnings
tools/dafny/dafny verify tests/verification/q1/dafny/Q1Maintenance.dfy
```
