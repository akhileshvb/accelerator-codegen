# Q12 verification status (ResultSpec path — Q1 strength)

## Gates

| Gate | Status |
|---|---|
| Logos lower `tpch_q12` | **green** (`formalsql/q12/generated/`) |
| `normalize(qhat_Q12)=Some(S_Q12_catalog)` | **green** (`compile_q12_resultspec.sh`) |
| `NORMALIZE_SOUND_Q12` | **green** (`compile_q12_normalize_sound.sh`) |
| `ORDER_MAINT_PROVED` | **green** (no `assume{:axiom}` on AdjustDays≡FoldLines) |
| `Q12ExecCorrect` (Inv ⇒ Exec == EvalResultSpec) | **green** (Compose morphism proved) |
| `MaintenancePreservesInv` | **green** (line + order `ReclassifyOrder`) |
| `Hit ⇒ Exec == EvalResultSpec` | **green** (`Q12RuntimeResultSpec`) |
| `RESULTSPEC_MODEL_EXPORT_OK_Q12` | **pin** (`check_spec_export_q12.sh`) |
| `FormalSQLAuxCorrect_Q12` | **not attempted** (ResultSpec path) |

## Architecture

```text
Inv(A, Orders, Lines)  ⇔  A = BuildFold(Orders, Lines)
Order ApplyDelta       ⇔  ReclassifyOrder (line-scan ≡ C++ reverse index)
```

## Remaining TCB

See [`ASSURANCE_Q1_STRENGTH_REPORT.md`](ASSURANCE_Q1_STRENGTH_REPORT.md).

```text
Logos lowering
FormalSQL-as-reference-semantics
Q12_GroupBagsPerm          (FS spine ↔ Embed(Sort Spec bags); Spec ops Qed)
SpecExportCorrect
Corresponds_Q12(C++, Model)
toolchain / SPI cuts
```

No AVG TCB. No maintenance axioms. `Q12_FS_eq_EvalSpec` removed as hyp.

## Commands

```bash
scripts/run_verification_q12.sh
```
