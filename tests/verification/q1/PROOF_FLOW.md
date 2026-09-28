# Q1 verification proof flow

End-to-end correctness story for the TPC-H **Q1** accelerator: how catalog SQL
connects to FormalSQL and `AuxAnswerSpec` (Rocq), and how generated C++ connects
back to the same `AuxAnswerSpec` (Dafny + extract). This document records the
**proof flow actually followed**, the **TCB**, and **where proving time went**.

Companion docs:

| Doc | Role |
|-----|------|
| [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md) | **Canonical** current theorem / TCB summary |
| [`../PIPELINE.md`](../PIPELINE.md) | Cross-query architecture claim |
| [`../README.md`](../README.md) | Operational file / runner map |
| [`../formalsql/q1/STATUS.md`](../formalsql/q1/STATUS.md) | Live Rocq Qed table (points here for TCB) |
| [`../formalsql/PIN.md`](../formalsql/PIN.md) | Logos / FormalSQL pin |
| [`TWIN_AUX_HARD_BRANCHES.md`](TWIN_AUX_HARD_BRANCHES.md) | Twin HB loci only |
| [`SOURCE_MAP.md`](SOURCE_MAP.md) | CorrespondsCppTwin pin |

Historical campaign notes ([`ASSURANCE_STRENGTHEN_REPORT.md`](ASSURANCE_STRENGTHEN_REPORT.md),
[`ASSURANCE_TCB_REDUCE_REPORT.md`](ASSURANCE_TCB_REDUCE_REPORT.md),
[`RESULTSPEC_POC_STATUS.md`](RESULTSPEC_POC_STATUS.md)) must not override
[`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md).

---

## 0. The meeting point: `AuxAnswerSpec`

Everything is designed to meet at one Spec answer:

```text
AuxAnswerSpec(DB, θ)
  = OutRowSort( SpecFinalize( SpecView( FoldSpec(DB), θ ) ) )
```

- **θ** = shipdate cutoff day (`EmbedParams_Q1(delta) = 10561 − delta` in Rocq;
  catalog SQL uses `delta = 90`).
- **Rocq** proves (under residuals / TCB hyps below) that FormalSQL’s successful
  observation of `q̂_Q1(δ)` on an embedded DB is `EmbedResult(AuxAnswerSpec …)`.
- **Dafny** proves Twin ops ≡ AuxSpec ops, so `FoldTwin = FoldSpec`, and that
  Inv / maintenance / runtime hit paths observe `AuxAnswerTwin = AuxAnswerSpec`.
- **C++** is tied to Dafny twins by structural twinning + finite extract-diff,
  not by a universal compiler proof.

So the stack is two arrows into one Spec, not a single kernel theorem across
Rocq and Dafny.

```text
        Catalog SQL
             │ Logos lower
             ▼
          q̂_Q1(δ)  ──Rocq FormalSQL──►  EmbedResult(AuxAnswerSpec)
                                             ▲
        generated C++                        │
             │ RepPhys + extract-diff        │ Twin≡Spec + Fold
             ▼                               │
        Dafny twins ──FoldTwin / Inv / RuntimeAuxCorrect──┘
```

---

## 1. Downward path: SQL → FormalSQL → AuxAnswerSpec

### 1.1 Catalog SQL → Logos → FormalSQL AST

1. Start from pinned TPC-H Q1 catalog `base_sql` (shipdate filter with
   `DATE '1998-12-01' - INTERVAL '<delta>' DAY`, group by returnflag/linestatus,
   SUM/AVG/COUNT, order by keys).
2. Run Logos lower (`scripts/logos_lower_catalog.sh q1`) — **fail closed** if
   unsupported.
3. Emit Rocq under `formalsql/q1/generated/` (`Queries.v`, `Schema.v`, …) and
   shared AuxSpec under `formalsql/generated/Q1AuxSpec.v` (hash-checked dual
   emission with Dafny `q1/dafny/generated/Q1AuxSpec.dfy`).

**Templatization:** Rocq defines `qhat_Q1(delta)` so the WHERE predicate uses
`CstZ(-delta)`. Catalog instance lemma:

```text
qhat_Q1 90 = Queries.source_query_expr   (Qed)
```

Theorems are **∀ `DeltaOk_Q1 delta`**, not only 90.

### 1.2 Embed bridge (Aux rows ↔ FormalSQL tuples)

In `Q1FormalSQLProof.v`:

| Map | Meaning |
|-----|---------|
| `EmbedRow_Q1` / `EmbedDB_Q1` | AuxSpec lineitem row → FormalSQL `TNull` tuple / DB |
| `EmbedParams_Q1(delta)` | cutoff day = base − delta |
| `EmbedOutRow_Q1` / `EmbedResult_Q1` | Aux `OutRow` → FormalSQL output tuple / list |
| `embed_avg_from_sum_scale2` | AVG cell uses FormalSQL `numeric_div_at_scales`, **not** Twin `AvgRound` |

**Important numeric split:** AuxSpec `OutRow.out_avg_*` stores `AvgRound` for
Twin correspondence; FormalSQL Embed AVG cells are built from **sum+count** via
`embed_avg_from_sum_scale2`. Rocq documents the mismatch
(`Q1_avg_formalsql_vs_avground_mismatch`). That split drove a large fraction of
AuxCorrect difficulty.

### 1.3 Semantic obligations (shape / Spec-side)

`Q1Obligations.v` + `Q1AuxCorrectAssembly.v` discharge Spec-side bridges such as:

- FilterEq — shipdate ≤ cutoff ↔ SpecView membership after FoldSpec
- GroupEq / Sum\*Eq / CountEq — SpecView key stats = folds over filtered key buckets
- OrderEq — `AuxAnswerSpec` sortedness / permutation vs `OutRowSort`
- Catalog hash / shape obligations

These are **AuxSpec Z-math and embedding lemmas**, not FormalSQL evaluator steps.

### 1.4 FormalSQL spine (TFGOP leaves)

Q1’s lowered AST is a Table → Filter → Project → Group → Order → Project spine
(**TFGOP**). The proof strategy was:

1. **Generic once:** `QuerySpineFacade.v` — compose `EQuery_*` for TFGOP.
2. **Per leaf Prop** in `Q1FormalSQLSpine.v` — success + exact observation:
   - `Q1_FilterRowsOk`
   - `Q1_InnerProjectOk` (dots + `$f4`/`$f5`)
   - `Q1_GroupBagOk` (SUM/AVG/COUNT)
   - `Q1_OrderOk`
   - `Q1_OuterProjectOk`
3. **Glue:** `FormalSQLAuxCorrect_Q1_of_leaves` — leaf successes ⇒
   `FormalSQL_exec … EmbedResult(AuxAnswerSpec)` **if** the final row list
   equals that EmbedResult.
4. **Abstraction rule used throughout:** if a leaf needed deep FormalSQL
   constructor grind (`EFilterRows_Cons`, `EScalar_*`, `EGroups_*`), **stop** and
   factor a reusable facade under `formalsql/lib/`, then thin-instantiate for Q1.

Facades built for that path:

| Facade | Purpose |
|--------|---------|
| `FilterAcceptanceFacade` | filter success ↔ `List.filter` via acceptance |
| `TemporalPredAcceptanceFacade` | DATE ≤ CAST+INTERVAL pred ↔ `Z.leb` |
| `NumericCallExactFacade` / `NumericScaledFits` | `$f4`/`$f5` Mul/Sub/Cast + digit mono |
| `NumericPackagingFacade` | SUM/AVG runtime-None packaging |
| `ProjectExactFacade` | project ↔ `map project_row` |
| `OrderRowsFacade` / `KeyedSortFacade` | canonical order / sorted uniqueness |
| `GroupBagExactFacade` / `GroupsExactFacade` | group bag from exact groups |
| `AggregateLeafExactFacade` | COUNT-star / SUM(dot) / AVG(fixed) leaf exact |

### 1.5 Leaf holds → AuxCorrect (current shape)

**Proved from Pre (not TCB):** full per-bucket `q1_group_bucket_safe`
(COUNT + SUM×4 + AVG×3) via digit-budget / packaging facades.

**Qed:** all five leaf holds; `FormalSQLAuxCorrect_Q1_of_holds` under final-list
equality; `Q1_GroupsCorrespond_holds` under a narrow bag residual; D outer
bridge (`q1_final_rows_eq_of_groups`).

**Current top theorem** (`Q1TCBAuxCorrect.v`):

```text
FormalSQLAuxCorrect_Q1 :
  Preconditions_Q1 db delta →
  Q1NumericSafe db →
  NumericEmbedFaithful →          (* TCB: numeric representation *)
  FormalSQL_exec (EmbedDB db) delta
    (EmbedResult (AuxAnswerSpec db (EmbedParams delta)))
```

`Q1_GroupBagsPerm` is **Qed** (`Q1_GroupBagsPerm_holds`) and is not a hyp of the
final theorem. `FormalSQLNoError_Q1` is separately Qed under Pre + `Q1NumericSafe`.

Canonical residuals: [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md).

### 1.6 Downward proof flow (chronological / dependency)

```text
1. Pin Logos; lower Cat(Q1) → q̂; prove qhat_Q1 90 = catalog AST
2. Define Embed* / Pre / DeltaOk / RowOk / Q1NumericSafe
3. Qed Spec obligations (FilterEq … OrderEq) and SpecView Z-field bridges
4. Qed QuerySpineFacade TFGOP once
5. Factor leaf facades; Qed Filter / Order / Outer / Inner / Group holds
6. Discharge bucket Safe from Pre (digit budget + AVG finalize)
7. Bag↔key: FormalSQL groups ↔ Aux RowKey buckets (Qed)
8. Per-bucket select_values = OutRow embeds under NumericEmbedFaithful (Qed)
9. Sort / outer bridges (Qed); `Q1_GroupsCorrespond_holds` (GroupBagsPerm discharged)
10. `FormalSQLAuxCorrect_Q1` under Pre + Safe + `NumericEmbedFaithful`
```

Open downward residual: **`NumericEmbedFaithful`** only
(see [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md); `Q1_GroupBagsPerm` Qed).

---

## 2. Upward path: C++ accelerator → Dafny → AuxAnswerSpec

### 2.1 Generated C++ (physical)

Artifact under generated Postgres aux (finalize / module / maintenance). This is
**RepPhys**: the code the engine runs. It is **not** proved equal to FormalSQL
by Rocq.

### 2.2 Structural Dafny twins (1:1 with C++)

| File | Twin of | Role |
|------|---------|------|
| `q1/dafny/Q1Finalize.dfy` | `finalize.cpp` | Aggregate finalize + avg closed form |
| `q1/dafny/Q1ModuleTwin.dfy` | `module.cpp` | Orchestration / Execute / StageMaintenance case-split |
| `q1/dafny/Q1Aux.dfy` | Algebra | `FoldTwin`, **`Inv(store,t) ⇔ store = FoldTwin(t)`** |
| `q1/dafny/Q1Maintenance.dfy` | StageMaintenance | **`MaintenancePreservesInv`** |
| `q1/dafny/Q1MaintenanceEdges.dfy` | Install/CAS/policy | Edges around Inv |
| `q1/dafny/Q1RuntimeAuxCorrect.dfy` | Runtime hit path | Hit ⇒ AuxAnswerTwin(VisibleDB) |

**Inv under maintenance** (the question “state under maintenance”):

- Defined in `Q1Aux.dfy`: `Inv(store, t) ⇔ store == FoldTwin(t)`.
- Lifted to aux state in `Q1Maintenance.dfy`: `StateInv(s) = StateOk(s) && Inv(s.store, s.table)`.
- Main lemma: `MaintenancePreservesInv` for NoOp / ApplyDelta\* / Rebuild\* /
  InvalidateAndFallback.
- Edges (delete-any, Publish/CAS, ChooseMaintenance → safe MaintAction) in
  `Q1MaintenanceEdges.dfy`.

### 2.3 Twin ≡ Spec → AuxAnswerSpec

| File | Claim |
|------|-------|
| `q1/dafny/generated/Q1AuxSpec.dfy` | Shared Spec ops / FoldSpec (generator twin of Rocq AuxSpec) |
| `q1/dafny/Q1TwinSpecBridge.dfy` | TwinOp ≡ SpecOp; **`FoldTwin = FoldSpec`**; AuxAnswerTwin = AuxAnswerSpec |

Once FoldTwin = FoldSpec, Inv and RuntimeAuxCorrect are about the **same** Spec
answer Rocq talks about (modulo embedding / hash bridge).

### 2.4 Substrate / runtime (shared)

Under `substrate/dafny/` and Postgres backend twins:

- PairInv / TokenForSnapshot / SnapshotHitSafe / ReadLinearizes /
  ConsistentObservation / LocalWriteSafe / InvalidateBreaksStaleCAS
- `RuntimeAuxCorrect`: AcceleratorHit ⇒ Execute(h) = AuxAnswerTwin(VisibleDB, θ)

### 2.5 C++ ↔ Dafny (finite, not ∀)

- `q1/extract/` — Dafny C# extract dump-and-diff vs generated C++ on **grid D**
- Live PostgreSQL differential backstop (`tests/aux_state/…`)
- Avgs: Dafny proves a `real` closed form; extract compares rational↔IEEE via
  `isclose` (not bit-identity)

### 2.6 Upward proof flow (dependency)

```text
1. Twin finalize/module functions 1:1 with C++
2. FoldTwin + Inv algebra (Q1Aux)
3. Twin≡Spec / FoldTwin=FoldSpec (Q1TwinSpecBridge)
4. MaintenancePreservesInv + edges
5. Substrate PairInv / snapshot / hit safety
6. RuntimeAuxCorrect (hit ⇒ AuxAnswerTwin)
7. Extract-diff on grid D + PG differential (confidence, not universal proof)
```

Deprecated as SQL authority (do not deepen): `Q1SqlSem.dfy` / `MathSem` in
`Q1Sql.dfy`. SQL meaning for the new stack is **FormalSQL only**.

---

## 3. Composed assurance claim (what “green” means)

```text
JetStreamFormalSQLCorrect (composed, not one cross-prover theorem):
  Rocq FormalSQLAuxCorrect_Q1   (under Pre + Safe + NumericEmbedFaithful)
    ⊕ Dafny Twin↔Aux closed package + RuntimeAuxCorrect
    ⊕ AuxSpec hash / dual-emission bridge
    ⊕ SpecExportCorrect + finite extract-diff / cpp_model
```

Proved edges **stop** at the Dafny/runtime model. C++ is RepPhys + finite check.
Full listing: [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md).

---

## 4. Trusted Computing Base (TCB)

**Canonical current list:** [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md).
Historical subsections below are retained for narrative; if they conflict, the
canonical file wins.

### 4.1 Assumed / not proved by this stack

| Item | Why it is TCB |
|------|----------------|
| FormalSQL as authoritative SQL semantics | Chosen SQL meaning; Logos publication ≠ proof of PG |
| Rocq / Dafny kernels | Proof checkers |
| Logos pin + lowering | Fail closed; lowering correctness assumed |
| EmbedDB / EmbedResult / EmbedParams **as maps** | Representation of Aux ↔ FormalSQL values |
| **`Q1NumericSafe DB`** | Domain+cardinality ⇒ no FormalSQL `SqlError` (overflow packaging; never values) |
| **`NumericEmbedFaithful`** | Z aggregate coefficients ↔ FormalSQL NUMERIC SUM/AVG cells (representation only; **does not** choose group membership) |
| PostgreSQL executor for supported fragment | Engine ≡ FormalSQL assumed for live runs |
| SPI/heap → abstract DB; snapshot S → DB(S) | Substrate abstraction |
| Twin ↔ C++ beyond extract-diff grid D | Universal compiler correctness not claimed (`cpp_model`) |
| AuxSpec generator (hash-checked dual emission) | Shared Spec text faithfulness |
| Unsupported FormalSQL/PG features | Fail closed — never approximated |
| Twin SPI: `AggsFaithful`; free `RebuildOracleCorrect` | Intentional; see canonical §2 |
### 4.2 Explicitly **not** TCB (proved or intended proved)

| Item | Status |
|------|--------|
| TFGOP spine facade + five leaf holds | Qed |
| Per-bucket overflow Safe from Pre | Qed (not TCB) |
| SpecView Z-field sum/count bridges | Qed |
| Bag↔key FormalSQL groups ↔ Aux RowKey | Qed |
| Per-bucket select_values = OutRow embeds (under Faithful) | Qed |
| Sort / outer finalize bridges | Qed (`Q1_GroupBagsPerm` discharged) |
| TwinOp ≡ SpecOp, FoldTwin = FoldSpec | Dafny Qed |
| Inv + MaintenancePreservesInv + edges | Dafny Qed |
| Twin↔Aux closed package (ComposeOutput, BuiltInv, HB1–HB4) | Dafny Qed — [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md) |
| RuntimeAuxCorrect model lemmas | Dafny Qed |

### 4.3 Narrow open residuals (honest)

| Residual | Kind | What it must **not** smuggle |
|----------|------|------------------------------|
| `NumericEmbedFaithful` | TCB hyp | Group membership / which rows contribute |
| `AggsFaithful` | Twin SPI | Unfinished Twin bookkeeping (it is intentional agg-scan morphism) |
| `RebuildOracleCorrect` (free oracle/load) | Twin SPI | Lineitem BuildState already discharges it |
| `cpp_model` / SpecExportCorrect / AvgObservation | Substrate / export / float8 | See canonical §3–§5 |

`Q1_GroupBagsPerm` is **Qed** — do not list it as open.

Do **not** TCB `q1_final_rows_of = EmbedResult(AuxAnswerSpec)` wholesale — that
would assume AuxCorrect.

---

## 5. Pain points and proving-time cost

Rough cost ordering from the Q1 FormalSQL campaign (largest first). Dafny Inv /
Twin≡Spec / extract were largely **already in place**; the bulk of recent LLM
token burn was the Rocq FormalSQL↔AuxSpec handshake.

### 5.1 Highest cost: FormalSQL ↔ AuxSpec **value** handshake

**Why:** two numeric worlds.

- AuxSpec: Z coefficients, `AvgRound`, SpecView association lists.
- FormalSQL: `numeric` embeds, scales 2/4/6, `interp_sum_numeric`,
  `interp_avg_numeric_fixed` / `numeric_div_at_scales`.

Proving “FormalSQL group select cells = Embed(OutRow)” required:

- `$f4`/`$f5` call trees + digit-budget mono (max-price → arbitrary price)
- AVG finalize packaging vs Twin AvgRound (documented mismatch)
- Bag↔key alignment (`query_make_groups` ↔ RowKey)
- SpecFinalize bag order vs FormalSQL group list order (`Q1_GroupBagsPerm`)

This is **most of AuxCorrect**, not a cleanup after the five leaves.

### 5.2 High cost: five TFGOP leaves as abstraction tax

**Why:** instruction was to treat leaves as a test of abstraction — factor
reusable facades instead of Q1-specific `EFilterRows_Cons` / `EGroups_*` grind.

Time went into:

- Filter acceptance + temporal DATE≤TIMESTAMP facade
- Numeric call exactness for `$f4`/`$f5`
- AVG(fixed) closed-group leaf exact in AggregateLeafExactFacade
- GroupsExact / Project / Order facades

**Payoff:** later queries can reuse facades. **Cost:** Q1 looked “stuck” while
building infrastructure.

### 5.3 Medium cost: overflow Safe packaging

Originally expected to be TCB-only. In the end **COUNT+SUM+AVG Safe was Qed
from Pre** via digit-budget / `fixed_decimal_*` / packaging facades — valuable
but slow (lots of Rocq `vm_compute` / lia / conforms lemmas).

### 5.4 Lower cost (already mature on Dafny side)

- Twin≡Spec / FoldTwin=FoldSpec
- Inv + MaintenancePreservesInv + Install/CAS edges
- Extract-diff grid + PG differential

These were not the recent bottleneck.

### 5.5 Process pain (LLM ↔ Rocq)

- Long compile cycles (`compile_q1_proof.sh`)
- Easy to waste tokens grinding constructors instead of stopping to factor
- Dual models (`AvgRound` vs `div_at_scales`) caused repeated false paths
- STATUS / theorem shape churn as residuals were narrowed (list-eq →
  GroupsCorrespond → GroupBagsPerm + Faithful)

### 5.6 What we would redesign next time

To cut proving time without gutting Twin↔FormalSQL:

1. **Align AuxSpec numerics with FormalSQL embeds** (or make FormalSQL /
   shallow TFGOP denotation the Spec Twin talks to) — kill dual AVG/scale math.
2. Keep **facade-first** FormalSQL leaves (that part worked).
3. Keep **narrow TCB** for representation (`NumericEmbedFaithful`) and overflow
   (`Q1NumericSafe`), never for group membership or final-row list equality.
4. Prove **bag↔key / SpecView correspondence** as the only heavy per-query
   semantic obligation.

---

## 6. File map (Q1)

### Rocq FormalSQL

```text
tests/verification/formalsql/
  lib/          # reusable TFGOP / filter / numeric / group / sort facades
  generated/    # Q1AuxSpec.v
  q1/
    generated/  # Logos Queries/Schema/Witness
    Q1FormalSQLProof.v      # Embed, Pre, NoError, empty AuxCorrect
    Q1Obligations.v         # FilterEq … OrderEq
    Q1AuxCorrectAssembly.v  # SpecView Z bridges, Embed dots
    Q1FormalSQLSpine.v      # leaves, Safe from Pre, of_holds
    Q1NumericEmbed.v        # NumericEmbedFaithful (TCB Prop)
    Q1GroupsCorrespond.v    # bag↔key, GroupsCorrespond_holds
    Q1TCBAuxCorrect.v       # D + FormalSQLAuxCorrect_Q1
    STATUS.md
```

### Dafny / C++ confidence

```text
tests/verification/q1/dafny/
  Q1Aux.dfy, Q1Maintenance.dfy, Q1MaintenanceEdges.dfy
  Q1Finalize.dfy, Q1ModuleTwin.dfy
  Q1TwinSpecBridge.dfy, Q1RuntimeAuxCorrect.dfy
  generated/Q1AuxSpec.dfy
tests/verification/q1/extract/          # finite C++ ≡ extract
tests/verification/substrate/dafny/     # PairInv / snapshot / policy
```

### Gates

```bash
tests/verification/formalsql/scripts/compile_q1_proof.sh
tests/verification/formalsql/scripts/check_q1_rocq_no_admitted.sh
scripts/run_verification_q1.sh          # Dafny + extract stack
```

---

## 7. One-page summary

| Direction | Claim | Tool |
|-----------|-------|------|
| SQL → FormalSQL | Catalog lowers to `q̂_Q1(δ)` | Logos (TCB lowering) |
| FormalSQL → AuxAnswerSpec | Successful eval = EmbedResult(AuxAnswerSpec) | Rocq (under Pre + Safe + `NumericEmbedFaithful`) |
| C++ → Twin | Structural twins + extract-diff grid D | Dafny / C# (`cpp_model`) |
| Twin → AuxAnswerSpec | TwinOp ≡ SpecOp; FoldTwin = FoldSpec; ComposeOutput≡ExecSeqM | Dafny (closed package) |
| State under maintenance | `Inv(store,t) ⇔ store = FoldTwin(t)`; preserved by StageMaintenance | `Q1Aux` + `Q1Maintenance` + Twin close |
| Runtime hit | Execute(h) = AuxAnswerTwin(VisibleDB) | `Q1RuntimeAuxCorrect` + substrate |

**Most proving time:** Rocq FormalSQL↔AuxSpec numeric/group-obs handshake and
reusable leaf facades — not Dafny Inv/maintenance.

**Current residuals:** [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md)
(`NumericEmbedFaithful`, Twin SPI, `cpp_model`, SpecExport / AvgObservation).
`Q1_GroupBagsPerm` is Qed.
