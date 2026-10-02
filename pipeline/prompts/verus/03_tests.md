# Verus pipeline: adversarial tests ({{benchmark}} q{{query}})

Write the tests the accelerator must pass. They are frozen before the kernel
exists and the kernel is never allowed to weaken them. The pipeline runs them
differentially: after the build and after every transaction, the
accelerator's answer must equal the query's, at the same snapshot, for your
parameter tuples; and readers in other connections must see their own
snapshot while transactions commit.

Read `query/`, `spec/gen_spec.rs`, `inputs/q.inputs` and
`frozen/gen_glue.cpp` (what is captured). Explore the test database with
`verus_sql`. Write in `tests/`:

* `params.sql`: one parameter tuple per line as SQL literals separated by
  commas (e.g. `'SAUDI ARABIA'` or `DATE '1995-01-01', 24`), covering values
  with large, small and empty answers, edge values (empty strings, NULL,
  values that match nothing, boundaries).
* `txns/NNN.sql`: one write transaction per file (DuckDB SQL; statements
  separated by `;`). Cover every table the query reads with inserts, deletes
  and updates (including key columns, several columns at once, duplicates,
  rows that enter and leave the answer, rows that only matter through
  EXISTS/NOT EXISTS, changes that need a rescan), and target rows that are in
  answers: a write that never changes an answer tests nothing. Choose rows by
  SQL (subqueries), not hard-coded ids, so the transactions stay valid.
* `setup.sql` (optional): shape the data first (adversarial rows: duplicate
  keys, NULLs, collation traps, extreme values).
* `bench_txns/NNN.sql` (optional): realistic point writes for benchmarking
  (default: txns/).

Run `verus_validate_tests` until every tuple and transaction runs.

Feedback from the previous round: {{feedback}}

{{include:shared/rules.md}}
