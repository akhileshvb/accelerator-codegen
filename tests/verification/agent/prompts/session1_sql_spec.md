# Session 1 — SQL semantic verification

You are one Codex session. Do not start a second agent. This is repair round {{ROUND}} of {{REPAIR_ROUNDS}}. The driver starts another round when the gates are still red, and it passes this plan and the gate results back to you.

Before editing, write `{{PLAN}}`. Order the remaining obligations so the first item is the smallest slice that can be closed in this round. Finish that item so it compiles with `Qed`. Leave later items for the next round. Do not exit because a later item is still open.

```text
write the plan
→ close only the first open item
→ run the query's compile script
→ read only the first failing goal for that item
→ fix
→ rerun
```

Do not declare success while a gate script is red. Success is only what the deterministic driver rechecks after you exit. A separate critique agent reviews this session after the gates are green. When its feedback is present below, address every finding in the proof. Do not answer a finding with Admit, Axiom, or by moving the obligation into the TCB or into a precondition.

## Plan

{{PLAN_TEXT}}

## Gate results

{{GATE_STATUS}}

## Critique feedback

{{CRITIQUE}}

## Query

- Name: `{{QUERY}}`
- Catalog SQL: `{{CATALOG_SQL}}`
- FormalSQL AST (Logos output, already generated): `{{FORMALSQL_DIR}}`
- Schema stub: `{{SCHEMA_SQL}}`
- Canonical spec (read-only): `{{CANONICAL_SPEC}}`
- Reusable Rocq library: `tests/verification/formalsql/lib/`
- Pattern library: `{{PATTERN_CATALOG}}` and `tests/verification/patterns/`
- Q1 example: `tests/verification/formalsql/q1/` and `tests/verification/formalsql/resultspec/`

## Goal

The canonical spec was written by a separate agent. Do not edit `{{CANONICAL_SPEC}}`. Prove that the FormalSQL query matches that document, under explicit row-domain and successful-evaluation preconditions:

```text
Eval_FormalSQL(qhat_Q, D) = EmbedResult(EvalSpec(S_Q, D))
```

`QueryResultSpec` records are shallow descriptors. Also produce the executable semantics this query needs: input rows, join, filter, grouping key, per-row contribution, aggregates, final projection, ordering, and `EvalSpec` / `AuxAnswerSpec` / `FoldSpec` / `ViewSpec` / `FinalizeSpec` as applicable.

## Reuse order

Read `{{PATTERN_CATALOG}}` and `tests/verification/patterns/` before writing a proof. Use any Rocq or Dafny entry there, and any facade under `formalsql/lib/`, when it applies.

1. Existing catalog entry or library theorem (`ListNoDupFacade`, `KeyedSortFacade`, `SortedGroupResultFacade`, `AggregateLeafExactFacade`, and the other facades under `formalsql/lib/`).
2. If the lemma does not mention this query and a later query could use it, add it under `tests/verification/formalsql/lib/` or `tests/verification/patterns/`. Do not leave a general lemma only in the query file.
3. A query-specific leaf, kept in the query directory.
4. Do not add a broad whole-query theorem that restates the claim.

Do not copy generic Q1 infrastructure into a query file.

When this session changes proofs, write `{{REUSE_RECORD}}` before you exit:

```json
{
  "query": "{{QUERY}}",
  "session": "sql_spec",
  "reused": ["SortedGroupResultFacade"],
  "additions": [
    {
      "name": "LemmaName",
      "kind": "reusable",
      "path": "tests/verification/formalsql/lib/LemmaFile.v",
      "prover": "rocq",
      "generalizes": "what a later query can use this for"
    }
  ]
}
```

`kind` is `reusable` or `query_leaf`. A reusable path must stay under `formalsql/lib/`, `patterns/`, or `substrate/`. An empty `additions` list is correct when every new fact is query-specific or already in the catalog. The driver appends reusable entries to the catalog after the Rocq gates pass. Query leaves are not added to the catalog.

## Fixture rule

For `q1` and `q12`, the files already in the tree are the reference. If they already pass the gates below, do not rewrite them. If a gate is red, patch the smallest failure and leave passing files unchanged.

For any other query, nothing is seeded. Generate the Rocq development, `proof_manifest.rocq.json`, `allowed_assumptions.txt`, and the compile script named in the gates. The canonical spec is the only semantic target. Do not invent a second spec. Do not write the Dafny file list.

## Proof shape

```text
predicate correctness
→ projection/contribution correctness
→ same contributing rows
→ same grouped aggregate bag
→ Permutation
→ shared deterministic sorting theorem
→ exact final result
```

## Failure

These do not count as success:

- `Admit`, `Admitted`, `admit`, `Axiom`
- a hypothesis equivalent to "FormalSQL result = ResultSpec result"
- `Q1_GroupBagsPerm` or `Q12_GroupRowsPerm_CollectModes` left as a hypothesis of the final theorem

Domain, uniqueness, overflow/`NumericSafe`, and successful evaluation belong in the theorem statement as preconditions. `NumericEmbedFaithful` (Q1 only) is a numeric-representation premise, not a semantic residual.

`Admitted` on the item this round was supposed to close is not progress. Record any still-open later item in `{{PLAN}}` and exit so the driver can start the next round. Do not weaken the statement to make the checker accept it.

## Gates to run

{{GATE_COMMANDS}}

On success the driver must be able to observe:

```text
SQL_SPEC_PROVED
NO_QUERY_SEMANTIC_RESIDUAL
NO_ADMITTED
```
