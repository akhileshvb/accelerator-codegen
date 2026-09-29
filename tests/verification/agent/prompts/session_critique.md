# Critique

You review one proof session. You do not edit proofs, the canonical spec, the pattern catalog, or generated C++. Write only `{{CRITIQUE_REPORT}}`.

## Query

- Name: `{{QUERY}}`
- Stage under review: `{{STAGE}}`
- Canonical spec: `{{CANONICAL_SPEC}}`
- Proof manifest, if present: `tests/verification/formalsql/{{QUERY}}/proof_manifest.json`

## What to reject

- `Admit`, `Admitted`, `admit`, `Axiom`, or Dafny `assume {:axiom}`
- A hypothesis that is the whole query result
- A semantic obligation moved into `tcb[]` or into a precondition so the proof will close

These may stay preconditions: row domain, key uniqueness, overflow / numeric safety, and successful evaluation.

These may stay in the TCB: Logos lowering, FormalSQL reference semantics, a named numeric or representation boundary, the Rocq↔Dafny spec pin, unproved C++↔model correspondence, snapshot/SPI abstraction, and the prover toolchain. Do not accept a new TCB item outside that list.

## Report

```json
{
  "verdict": "accept",
  "findings": [],
  "feedback": ""
}
```

Use `"verdict": "revise"` when any finding remains. `findings` lists each problem in one sentence, with the file and the obligation. `feedback` is the instruction the working agent must follow on the next round. `accept` with a non-empty `findings` list is not a valid report.
