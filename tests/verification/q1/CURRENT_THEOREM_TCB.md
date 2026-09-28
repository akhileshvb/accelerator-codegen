# Q1 current theorem / TCB summary

**Canonical status for Q1 verification.** Other docs should link here rather than
restating proved vs open lists. Historical campaign reports keep narrative but
must not contradict this file.

Gates that must stay green with this summary:
`scripts/run_verification_q1.sh`, `formalsql/scripts/compile_q1_proof.sh`,
`formalsql/scripts/compile_q1_normalize_sound.sh`,
`formalsql/resultspec/check_spec_export.sh`,
`tools/dafny/dafny verify tests/verification/q1/dafny/Q1TwinAuxClose.dfy`.

Interpreter residual detail:
[`../formalsql/resultspec/GENERIC_INTERPRETER_TCB.md`](../formalsql/resultspec/GENERIC_INTERPRETER_TCB.md).

---

## 1. Proved Twin ↔ Aux (Dafny)

Primary artifacts: [`dafny/Q1TwinAuxRefinement.dfy`](dafny/Q1TwinAuxRefinement.dfy),
[`dafny/Q1TwinAuxClose.dfy`](dafny/Q1TwinAuxClose.dfy), [`dafny/Q1Aux.dfy`](dafny/Q1Aux.dfy).

| Claim | Status |
|-------|--------|
| `TwinAuxRel` / `Flatten` / `FoldTwinFromTwin = FoldTwin` | Qed |
| `ApplyDeltaSeq` / Stage ApplyDelta refine | Qed |
| `BuildStateForDays` **lineitem** ⇒ `RebuildOracleCorrect(FoldTwin(InputsToRows))` | Qed |
| Hit `Execute` = `ComposeOutput` | Qed |
| `DayPartToKeyStorePos(TwinMerged) == Compose` | Qed |
| `FinalizeSortFaithful` via `SameMapSameSortedSeq` (`FinalizeSortFaithful_from_map`) | Qed |
| `ComposeOutput == ExecSeqM` (no free finalize premise) | Qed |
| Fallback `Execute == ExecSeqM` under `BuiltInvAtCutoff` from BuildState/load producers | Qed |
| HB1 `requires_rebuild` store = Aux `NoOp` (action retag only) | Qed |
| HB4 delete/insert packaging via Fold + Twin deltas | Qed |
| `PartsWellFormed` / `OrderUnique` / `OrderExact` through ApplyDelta/Erase/Put/build/decode/load | Qed |
| Closed package: `Q1Twin_Execute_*_closed`, `Q1Twin_Maint_*_closed` | Qed |

Operational HB catalog (C++ loci only): [`TWIN_AUX_HARD_BRANCHES.md`](TWIN_AUX_HARD_BRANCHES.md).  
CorrespondsCppTwin pin: [`SOURCE_MAP.md`](SOURCE_MAP.md).

**Not** reintroduced: catch-all `twin_aux_refinement` TCB.

---

## 2. Intentional Twin runtime / SPI contracts

| Contract | Role | Not |
|----------|------|-----|
| **`AggsFaithful`** | Aggregate-scan oracle ≡ Fold of filtered table; needed only for agg `BuildState` arms | Unfinished bookkeeping; lineitem BuildState does not use it |
| **`RebuildOracleCorrect`** | Free Stage rebuild oracle / load-image content ≡ expected Aux store | Discharged for lineitem `BuildStateForDays` |

Former free contracts now lemmas: `FinalizeSortFaithful`, `BuiltInvAtCutoff` as Execute premise, PartsWF/OrderUnique stubs.

---

## 3. `cpp_model`

| Item | Status |
|------|--------|
| CorrespondsCppTwin | **TCB** — ModuleTwin structural twin + finite extract-diff / SOURCE MAP pin; **not** a universal C++↔Twin proof |

Runtime theorems prove the **model** endpoint (`ExecSeqM` ≈ generic `Eval_Spec` OutRows).  
C++ ↔ Twin / `ExecSeqM` remains this separate boundary — do not blur.

---

## 4. SQL-side trusted / premise boundaries (Rocq)

Top theorem shape (`FormalSQLSpecCorrect_Q1` / NormalizeSound corollaries):

```text
Preconditions_Q1 db delta →
Q1NumericSafe db →
NumericEmbedFaithful →
  FormalSQL_exec … = EmbedResult(Eval_Spec_Q1_outs …)
  where Eval_Spec_Q1_outs = unwrap(Eval_Spec_Q1_rows_rel)
       = unwrap(Q1Adapt ∘ Eval_Spec(LogicalSpec_Q1_rel, …))
  Q1ByteKeys discharged from Pre (not a public premise)
```

| Item | Kind | Notes |
|------|------|-------|
| **Logos** lowering / pin | TCB | Fail closed if unsupported; publication ≠ PG proof |
| **`Preconditions_Q1`** | Premise | Domain / delta / row well-formedness |
| **`Q1NumericSafe`** | Premise (packaging) | Overflow Safe; bucket Safe Qed from Pre — not value TCB |
| **`NumericEmbedFaithful`** | TCB hyp | Z aggregate coeffs ↔ FormalSQL NUMERIC cells; does **not** choose groups |
| **`Q1_GroupBagsPerm`** | **Qed** (`Q1_GroupBagsPerm_holds`) | Discharged via SortedGroupResultFacade — **not** open |
| FormalSQL as SQL semantics | Chosen TCB | Engine ≡ FormalSQL for supported fragment assumed on live runs |
| **`Q1ByteKeys`** | Derived | Proved from Pre; not TCB / not public hyp |

Live Rocq Qed table: [`../formalsql/q1/STATUS.md`](../formalsql/q1/STATUS.md).

---

## 5. Cross-language / Spec-export boundaries (Phase 4 + RelNode IR)

**Meeting point (Rocq public, proved):** RelNode `LogicalSpec_Q1_rel` + RelNode
`Eval_Spec` + RelNode `Q1Adapt` (`Eval_Spec_Q1_rows_rel`). Observational
equality to flat Eval∘Adapt / AuxAnswerSpec is Qed in
[`Q1RelNodeBridge.v`](../formalsql/q1/Q1RelNodeBridge.v).

**Dafny public Exec:** RelNode `Eval_Spec_Q1_OutRows_rel` (Qed observational
bridge to flat / EvalResultSpec in
[`Q1RelNodeBridge.dfy`](dafny/Q1RelNodeBridge.dfy)). RelNode surface:
[`Q1RelNodeSpec.dfy`](dafny/Q1RelNodeSpec.dfy). Flat `Eval_Spec_Q1_OutRows` is
migration scaffolding only.

Query-specific *logical-query* semantics live in RelNode LogicalSpec DATA
(Rocq `LogicalSpec_Q1_rel` → `Q1RelNodeData.dfy`, hash-pinned).
`Q1Adapt` / `AvgObservationFaithful` remain query-specific representation / observation bridges.

| Item | Kind | Notes |
|------|------|-------|
| **SpecExportCorrect (A)** | Deterministic RelNode DATA transcription | Rocq `LogicalSpec_Q1_rel` → JSON → `Q1RelNodeData.dfy`; regen+diff+hash. **Not** a semantic proof. Gate: `check_relnode_logical_spec_export.sh` via `check_spec_export.sh`. Flat `LogicalSpecData.dfy` is migration scaffolding only. |
| **SpecExportCorrect (B)** | TCB `rocq_dafny_spec` (RelNode primary) | `formalsql/logical/*` ↔ `logical/dafny/*` via `interpreter_correspondence_manifest.txt` + **`interpreter_source_relnode.pin`**. Dual presence is **not** semantic equivalence. See [`GENERIC_INTERPRETER_TCB.md`](../formalsql/resultspec/GENERIC_INTERPRETER_TCB.md). |
| **Query adapters (C)** | Separate residual correspondence | Rocq `Q1Adapt` ↔ Dafny `Q1AdaptRel` (+ `RowOfAux` / `DbOfAux` / `ParamCutoff`). Presence only — **not** semantic equivalence; being outside (B) does **not** prove adapters. |
| **Flat migration** | Scaffolding | `*_migration_flat.*` pins/manifest/vocab + `check_logical_spec_export.sh` |
| **Migration lemmas** | Qed (query-specific) | `Eval_Spec` ↔ AuxAnswerSpec / EvalResultSpec under Pre — scaffolding toward the generic endpoint |
| AuxSpec dual emission | Generator faithfulness | Twin/Aux layer; unchanged |
| **`AvgObservationFaithful`** | TCB | float8 AVG observation; exact SUM/COUNT/keys/order proved |
| **`NormalizeSound_Q1` / generic** | Qed | Target = Eval_Spec OutRows |

### Demoted (scaffolding only — not Spec authority)

`EvalSpec_Q1`, `EvalResultSpec`, `SpecApply`, `SpecFinalizeOne`, `FinalizeKeys`, `SortByKey`, tag-era `Q1ResultSpecData` / `QueryResultSpec_Q1`. Files retained while migration proofs depend on them; headers mark scaffolding.

---

## 6. One-page composed claim

```text
JetStreamFormalSQLCorrect (composed, not one cross-prover theorem):
  Rocq FormalSQLSpecCorrect_Q1 / NormalizeSound_Q1
       [hyps: Pre, Q1NumericSafe, NumericEmbedFaithful]
       [target: EmbedResult(Eval_Spec_Q1_outs) = RelNode Q1Adapt∘Eval_Spec]
    ⊕ Dafny Q1ExecCorrectOrdered ≈ RelNode Eval_Spec_Q1_OutRows_rel
       [Qed bridge: OutRows_rel == EvalResultSpec == flat OutRows]
       [flat EvalLogicalSpec retained as migration scaffolding only]
    ⊕ Dafny Twin↔Aux closed package + Runtime/Exec/Maint
       [SPI: AggsFaithful on agg BuildState; RebuildOracleCorrect on free oracle/load]
    ⊕ SpecExportCorrect [RelNode DATA pin + RelNode interpreter TCB + adapters + flat migration]
    ⊕ cpp_model [extract-diff / SOURCE MAP]
```

Proved model edges stop at Dafny twins + Aux/generic Spec. C++ beyond extract-diff remains `cpp_model`.
