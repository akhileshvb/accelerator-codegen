# Q21 FormalSQL status

```text
Cat(Q21) --Logos lower--> qhat_Q21 (= query_spine_join4)
        |
        | Qed obligations + leaf Props
        v
 QueryJoinSpineFacade → AuxAnswerSpec(units, θ)
        |
        | Dafny Twin≡Spec → runtime → C++
```

**Parameter:** `theta` = nationkey (catalog SQL fills `n_name = 'SAUDI ARABIA'`;
AuxSpec `SpecView` is currently identity). Claim for empty units is **Qed**;
nonempty EXISTS↔sole-late after multi-table Embed remains open.

## Live Rocq files

| File | Role |
|------|------|
| `../lib/QueryJoinSpineFacade.v` | Q21 CrossJoin ladder `query_spine_join4` once |
| `../lib/QuerySpineFacade.v` | Q1 TFGOP (shared lib) |
| `Q21FormalSQLProof.v` | Embed, `qhat_Q21`, Pre, NoError, empty AuxCorrect |
| `Q21Obligations.v` | ContribEq / FoldSpec_nil / CatalogHash / QhatShape |
| `Q21FormalSQLSpine.v` | Leaf Props + empty of_leaves |

## Qed

| Item | Status |
|------|--------|
| `qhat_Q21_catalog_instance` | **Qed** |
| `Q21ContributionBridge` / `ContribEq_Q21` | **Qed** |
| `FormalSQL_empty_db_success_nil` | **Qed** (join-spine; EXISTS unevaluated on nil) |
| Empty-DB AuxCorrect (`FormalSQLAuxCorrect_Q21_nil`) | **Qed** |
| `FormalSQLAuxCorrect_Q21_of_empty_leaves` | **Qed** |
| Empty table / filter / group / order / outer leaves | **Qed** |
| `FormalSQLNoError_Q21` (success-observation form) | **Qed** |

## Open (nonempty units)

| Leaf | Status | Remaining obligation |
|------|--------|----------------------|
| Multi-table `EmbedDB` packing from units + θ | open | supplier/orders/lineitem/nation rows |
| `ExistsEq` FormalSQL ∃/¬∃ ↔ Spec sole-late | open | correlated EXISTS on EmbedDB |
| `NameJoinEq` (suppkey,numwait) ↔ (s_name,numwait) | open | Finalize name packaging |
| Universal `FormalSQLAuxCorrect_Q21` | open | needs above three |

## TCB

> Empty four-table witness for `units = nil`. Nonempty multi-table Embed is the
> representation bridge (not value oracle). Do not hand-author FormalSQL query terms.

## Gates

`compile_q21_proof.sh` · `check_q21_rocq_no_admitted.sh` · Dafny `Q21TwinSpecBridge.dfy`
