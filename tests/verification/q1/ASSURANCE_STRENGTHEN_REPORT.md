# Q1 assurance strengthening report (post ORDERING_PROVED)

Do not start Q6 — this task stops after A–D below.

---

## Phase A — NormalizeSound (`NORMALIZE_SOUND_OK`)

### What was done

| File | Role |
|------|------|
| `formalsql/resultspec/EvalSpec.v` | `EvalSpec_Q1 s db := AuxAnswerSpec db (qrs_cutoff s)` |
| `formalsql/q1/Q1NormalizeSound.v` | `NormalizeSound_Q1`, `Normalize_and_Sound_Q1`, nil case |
| `formalsql/resultspec/NormalizeTCB.v` | **retired** (empty marker) |
| `formalsql/scripts/compile_q1_normalize_sound.sh` | gate script |

### Theorem

```text
NormalizeSound_Q1:
  Pre → Q1NumericSafe → NumericEmbedFaithful → Q1_GroupBagsPerm →
  FormalSQL_exec(EmbedDB(db), δ)
    = EmbedResult(EvalSpec_Q1(S_Q1(cutoff(δ)), db))
```

Discharged by rewriting to `FormalSQLAuxCorrect_Q1` — **no leaf reopen**.

### Effort

| Metric | Value |
|--------|------:|
| New Rocq theorems | 3 |
| Manual proof LOC | ~15 (rewrites) |
| Solver/auto | trivial |
| Reused | `FormalSQLAuxCorrect_Q1`, `QuerySpineFacade`, full leaf stack |
| Compile pain | Low (uses existing `.vo`; full AuxCorrect rebuild currently hits unrelated `Q1GroupsCorrespond` flake — not introduced here) |

### Did facades prevent AuxCorrect-style campaign?

**Yes.** NormalizeSound is a thin alias of the already-proved AuxCorrect endpoint. No AggregateLeafExact / GroupsExact work.

### Scope honesty

- Proved for **`qhat_Q1 δ`** (supported family).
- Generic `∀q. normalize(q)=Some(s) → …` **not** claimed: normalizer peels TFGOP+lineitem+delta only (does not fingerprint selects). Future hardening: strengthen normalize.
- Inherits AuxCorrect residuals: `NumericEmbedFaithful` [TCB], `Q1_GroupBagsPerm` [hyp].

### TCB removed

```text
NORMALIZE_TCB  →  GONE (replaced by NormalizeSound_Q1)
```

---

## Phase B — SpecExport cleanup

### Assessment

Preferred “generic Dafny EvalSpec + Spec data only” is **not worth doing for Q1 alone**:

- Dafny `EvalResultSpec` (KeysFrom/Fold/Finalize) is already the working model endpoint.
- Generic interpreter would rewrite Q1 for zero immediate amortization until Q6+.

### What changed

- Rocq `EvalSpec.v` now exists (shared with NormalizeSound).
- Export gate checks `EvalSpec.v` presence + pin.
- **SpecExportCorrect remains TCB** (pin-checked dual Rocq↔Dafny).

### Trusted surface

Unchanged in size: pin + dual semantics. Slightly clearer: Rocq has an explicit EvalSpec definition; Dafny is the model twin.

---

## Phase C — Runtime / snapshot composition

### Files

| File | Role |
|------|------|
| `q1/dafny/Q1RuntimeResultSpec.dfy` | `SnapshotInv`, `Hit_implies_ExecApproxResultSpec`, `Q1RuntimeCorrect_from_Hit` |
| `q1/dafny/Q1Maintenance.dfy` | `MaintTwoStepPreservesInv` (reachability) |

### Theorems

```text
SnapshotInv:
  AcceleratorHit ∧ PairInv ∧ SnapshotHitSafe
    ⇒ Inv(fold_pub, DB(S)) ∧ VisibleDB = DB(S)

Q1RuntimeCorrect_from_Hit:
  Hit ∧ … ⇒ ApproxQ1(ExecSeqM(DB(S), p), EvalResultSpec(DB(S), p))
```

Cross-language to FormalSQL: compose with `NormalizeSound_Q1` + SpecExportCorrect [TCB] + Corresponds_Q1 [TCB].

### Effort

| Metric | Value |
|--------|------:|
| New Dafny lemmas | 4 (+ 1 maint) |
| Reused | `Q1RuntimeAuxCorrect` PairInv/SnapshotHitSafe, `Q1ExecCorrectOrdered` |
| Verify | Runtime 6, Maint 34, Bridge 74 — all green |

### Gates

`SUBSTRATE_PROVED`, `RUNTIME_EXEC_PROVED` — **green**.

---

## Phase D — AvgObservationFaithful assessment

### Feasibility: **Option B — keep TCB**

| Option | Cost | Recommendation |
|--------|------|----------------|
| A. Prove `HugeintScaleToDouble ≈ ExactAvg` under AbsRel | Needs IEEE-754 / Flocq-style model or custom error analysis; not in current toolchain | **Do not** |
| B. Keep `AvgObservationFaithful` | Zero cost; numerators/counts already proved exact | **Keep** |

Estimated surface if forced: new double model + scale/div lemmas + ≈ AbsRel wiring — multi-day, high solver risk. **Does not amortize** across queries better than a named observation TCB.

Exact SUM/COUNT/keys/order remain proved; only float8 AVG observation is trusted.

---

## Final Q1 TCB (meaningful)

```text
Logos lowering
FormalSQL chosen semantics
NumericEmbedFaithful          (representation; from AuxCorrect)
Q1_GroupBagsPerm              (narrow hyp; from AuxCorrect)
SpecExportCorrect             (Rocq↔Dafny pin dual)
Corresponds_Q1(C++, Model)
AvgObservationFaithful        (float8 observation)
proof checker / SPI substrate cuts
```

**Removed this task:** `NORMALIZE_TCB`.

---

## Success criterion check

| Goal | Met? |
|------|------|
| Generic reusable semantic claim out of TCB | Yes — NormalizeSound |
| SQL-result / Exec / Maint / Snapshot proved | Yes (model path) |
| Mechanical cross-language / float kept trusted | Yes |
| No Q6 started | Yes |
