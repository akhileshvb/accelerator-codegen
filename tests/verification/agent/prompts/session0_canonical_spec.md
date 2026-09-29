# Canonical spec

You are one Codex session, and you are not part of either proof effort. Do not start another agent. Do not prove anything in Rocq or Dafny.

Write the mathematical query result as a single JSON document. Both later proof sessions must target this document. You must not shape it to make either prover easier.

## Inputs you may read

- Catalog SQL: `{{CATALOG_SQL}}`
- Schema: `{{SCHEMA_SQL}}`
- Logos FormalSQL query term: `{{FORMALSQL_DIR}}` (`Queries.v`, `Schema.v` only)

## Do not read

- Rocq proof files (`tests/verification/formalsql/**/*.v` other than the Logos `generated/` query term)
- Dafny files
- Generated C++ under `src/generated/`
- Existing `EvalSpec`, `ResultSpec`, or accelerator models

Those artifacts are produced to match this spec. Reading them would bias the spec toward one side.

## Output

Write only:

```text
{{CANONICAL_SPEC}}
```

If that file already exists and already describes this query, leave it unchanged.

Required JSON fields:

```text
query
inputs
parameters
join
filter
group_key
contributions
aggregates
final_projection
ordering
```

`join` is `null` when the query reads one table. State parameters, predicates, grouping keys, per-row contributions, aggregates, the final projection, and ordering in ordinary mathematical language. Do not emit Rocq or Dafny.

Do not edit any other file.
