# Verus pipeline: audit the generated obligations ({{benchmark}} q{{query}})

`frozen/gen_obligations.rs` was generated from the declared kernel inputs
(`inputs/q.inputs`, rules in `contracts/inputs_language.md`) by the same
translator that produced the spec. The kernel's proof assumes these
contracts; the host glue is checked against the declared SQL. So each
contract must mean exactly what its declaration's SQL means.

For each declaration, compare the SQL with its contract:

* `scan_<name>_ok(db, rows)`: the rows are, as a bag, the SELECT's result on
  `db` (the table, its WHERE including `[NOT] EXISTS`, the projected columns
  and their runtime types: decimal scale, dates as days, text as bytes);
* `rescan_<name>_ok(db, keys, rows)`: the same, restricted to rows whose KEYS
  column is in `keys`;
* the delta views and `<table>_delta_ok` (lossy): each runtime field is the
  declared expression of the changed row; `capture_ok`: every table of `db2`
  is `db`'s plus the signed changes;
* the `ob_*` signatures: what each requires and ensures.

Use `verus_sql` to settle a question with data. Write
`analysis/obligations_audit.json`:
`{"verdict": "ok" | "issues", "issues": [{"declaration": "...", "sql": "...", "contract": "...", "why": "..."}]}`.
Only real differences in meaning ("issues" stops the pipeline).

{{include:shared/rules.md}}
