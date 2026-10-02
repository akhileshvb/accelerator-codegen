# Verus pipeline: audit the generated spec ({{benchmark}} q{{query}})

`spec/gen_spec.rs` was generated from `query/query.sql` (with
`query/schema.sql`) by a deterministic translator. Every proof downstream is
checked against it, so it must mean exactly what the SQL means.

Read the SQL, then the spec's definitions (`Db`, `Params`, the row and
helper functions, `query_result` and what it requires of `out`). Check:

* the rows and columns each table contributes, as bags (duplicates kept);
* every predicate: comparisons, `EXISTS`/`NOT EXISTS` and their correlation,
  `IN`, `LIKE`, `CASE`, arithmetic and its types (decimal scales, dates);
* NULL semantics (three-valued logic, NULLs in aggregates and comparisons);
* grouping keys, aggregates (COUNT/SUM/AVG/MIN/MAX, DISTINCT), HAVING;
* the output: columns, ORDER BY (direction, ties), LIMIT, text comparison
  (byte order);
* parameters ($n) and their types.

You may run read-only SQL on the test database (`verus_sql`) to settle a
question with data.

Write `analysis/spec_audit.json`:
`{"verdict": "ok" | "issues", "issues": [{"where": "...", "sql": "...", "spec": "...", "why": "..."}], "notes": "..."}`.
Only report real differences in meaning ("issues" stops the pipeline: the
translator must be fixed).

{{include:shared/rules.md}}
