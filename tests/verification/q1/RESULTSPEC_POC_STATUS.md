# Q1 assurance status (NormalizeSound + runtime composition)

## Gates

| Gate | Status |
|------|--------|
| `RESULTSPEC_NORMALIZE_OK` | **green** — `normalize(qhat_Q1 δ)=Some(S_Q1(cutoff δ))` |
| `NORMALIZE_SOUND_OK` | **green** — `NormalizeSound_Q1` (replaces `NORMALIZE_TCB`) |
| `RESULTSPEC_MODEL_EXPORT_OK` | **green** — SpecExportCorrect TCB (pin); Rocq `EvalSpec.v` present |
| `EXEC_PROVED` | **green** — per-group `Q1ExecCorrect` |
| `ORDERING_PROVED` | **green** — `Q1ExecCorrectOrdered` |
| `MaintenancePreservesInv` | **green** (+ `MaintTwoStepPreservesInv`) |
| `SUBSTRATE_PROVED` | **green** — `SnapshotInv` in `Q1RuntimeResultSpec.dfy` |
| `RUNTIME_EXEC_PROVED` | **green** — `Q1RuntimeCorrect_from_Hit` |

## Theorem chain (proved vs TCB)

```text
Logos(Q1)=qhat_Q1                         [TCB]
normalize(qhat)=Some(S_Q1)                [PROVED]
Eval_FS(qhat,D) = Embed(EvalSpec(S_Q1,D)) [PROVED NormalizeSound_Q1]
                                              inherits Faithful + GroupBagsPerm
SpecExportCorrect(S_Q1, S^M)              [TCB — pin-checked dual]
Inv ⇒ ExecSeq ≈_Q1 EvalResultSpec         [PROVED ORDERING]
Inv ⇒ Inv(M(A,u), Apply(D,u))             [PROVED]
Hit ⇒ Inv(A, DB(S))                       [PROVED SnapshotInv]
Hit ⇒ ExecSeq ≈_Q1 EvalResultSpec(DB(S))  [PROVED Q1RuntimeCorrect]
Corresponds_Q1(C++, Model)                [TCB]
AvgObservationFaithful                    [TCB — kept; see AVG assessment]
```

## Commands

```bash
tests/verification/formalsql/scripts/compile_q1_normalize_sound.sh
tests/verification/formalsql/resultspec/check_spec_export.sh
tools/dafny/dafny verify tests/verification/q1/dafny/Q1ResultSpecBridge.dfy
tools/dafny/dafny verify tests/verification/q1/dafny/Q1Maintenance.dfy
tools/dafny/dafny verify tests/verification/q1/dafny/Q1RuntimeResultSpec.dfy
```

Full phase report: [`ASSURANCE_STRENGTHEN_REPORT.md`](ASSURANCE_STRENGTHEN_REPORT.md)
