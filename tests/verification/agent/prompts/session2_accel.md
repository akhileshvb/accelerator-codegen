# Session 2 — accelerator verification

You are one Codex session. Do not start a second agent. This is repair round {{ROUND}} of {{REPAIR_ROUNDS}}. The Rocq session runs at the same time and does not have to finish first. The driver starts another Dafny round when these gates are still red, and it passes this plan and the gate results back to you.

Before editing, write `{{PLAN}}`. Order the remaining obligations so the first item is the smallest slice that can be closed in this round: the model, then maintenance, then the snapshot/hit proof. Finish that item so `dafny verify` accepts it. Leave later items for the next round.

```text
write the plan
→ close only the first open item
→ run dafny verify on that file
→ read only the first failing obligation
→ fix
→ rerun
```

Do not declare success while a gate script is red. The driver rechecks after you exit. A separate critique agent reviews this session after the gates are green. When its feedback is present below, address every finding in the proof. Do not answer a finding with `assume {:axiom}` or by moving the obligation into the TCB.

## Plan

{{PLAN_TEXT}}

## Gate results

{{GATE_STATUS}}

## Critique feedback

{{CRITIQUE}}

## Read-only accelerator

Generated C++ under `{{ACCELERATOR}}` is read-only. You may create or edit Dafny proof and model files only. Do not modify, reformat, or regenerate the accelerator to make verification pass.

If the C++ disagrees with the semantic spec, stop, leave every accelerator file unchanged, and report the smallest violated obligation or a concrete counterexample. Repairing C++ is a separate workflow.

The driver hashes `{{ACCELERATOR}}` before and after this session and fails the run if anything there changes.

## Inputs

- Query: `{{QUERY}}`
- Canonical spec (read-only): `{{CANONICAL_SPEC}}`
- Accelerator (read-only): `{{ACCELERATOR}}`
- Accepted spec from Session 1: `{{SPEC_FILES}}`
- Q1 Dafny examples: `tests/verification/q1/dafny/`
- Pattern library: `{{PATTERN_CATALOG}}` and `tests/verification/patterns/`

## Goal

The canonical spec was written by a separate agent. Do not edit `{{CANONICAL_SPEC}}`. Inspect the C++ and synthesize or keep the proof model so that execution matches that spec:

```text
Inv(A,D) := A = BuildFold(D)
ExecModel(A,p) = EvalSpec(S_Q, D, p)
Maintenance(A,D,Δ) = (A',D')  ⇒  Inv(A',D')
Hit(A,S) ⇒ Inv(A, DB(S))
Hit(A,S) ⇒ ExecModel(A) = EvalSpec(S_Q, DB(S))
```

Use observational equivalence only when the representations differ, and name that relation. Prove maintenance for every supported maintenance action. Instantiate the existing snapshot/pinning runtime path.

## Reuse order

Read `{{PATTERN_CATALOG}}` and `tests/verification/patterns/` before writing a proof. Use any entry there, including Rocq facades, when the same fact applies to the Dafny model.

1. Partitioned grouped aggregate, join-aware reclassification, map/fold maintenance, sorted unique map, snapshot pinning, and any other catalog entry.
2. If the lemma does not mention this query and a later query could use it, add it under `tests/verification/patterns/` or `tests/verification/substrate/`. Do not leave a general lemma only in the query file.
3. A query-specific leaf, kept in the query directory.
4. Do not replace a maintenance or execution obligation with `assume {:axiom}`.

When this session changes proofs, write `{{REUSE_RECORD}}` before you exit:

```json
{
  "query": "{{QUERY}}",
  "session": "accel_verify",
  "reused": ["SortedUniqueMap"],
  "additions": []
}
```

`kind` is `reusable` or `query_leaf`. A reusable path must stay under `formalsql/lib/`, `patterns/`, or `substrate/`. The driver appends reusable entries to the catalog after the Dafny gates pass. Query leaves are not added to the catalog.

## Fixture rule

For `q1` and `q12`, if the Dafny files already pass the gates below, do not rewrite them. If a gate is red, patch the smallest failure and leave passing files unchanged.

For any other query, nothing is seeded. Generate the Dafny model, maintenance proof, and snapshot/hit proof, and list those files in `proof_manifest.dafny.json`. Do not modify the canonical spec, the Rocq files, or the C++ accelerator.

## Correspondence

`Corresponds(C++, Model)` is not proved by unit or differential tests. Do not report the generated C++ as fully formally verified. Finite extract-diff evidence may be cited only as finite evidence.

## Gates to run

{{GATE_COMMANDS}}

A maintenance or execution `assume {:axiom}` fails this stage.
