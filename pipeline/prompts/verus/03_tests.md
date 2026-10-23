# Verus pipeline: tests ({{benchmark}} q{{query}})

Write the tests the accelerator must pass. They are frozen before the kernel
exists and the kernel is never allowed to weaken them. The pipeline runs them
differentially: after the build and after every transaction, the
accelerator's answer must equal the query's, at the same snapshot, for your
parameter tuples; and readers in other connections must see their own
snapshot while transactions commit.

**Mostly a realistic workload, plus some edge cases.** Most of the tests
should look like the query's real use: normal parameter values, ordinary
writes of the kinds the application makes, in varied orders and mixes, so the
accelerator is exercised the way it will be in production. A smaller part
(roughly a quarter) covers edge cases. **Everything must satisfy the
schema** (`query/schema.sql`): its NOT NULL columns get values, types and
ranges are respected. A state the schema rules out is outside what the
accelerator is specified for, so it tests nothing useful; the pipeline
enforces the schema's NOT NULL constraints on the test databases, so such a
write fails validation.

Read `query/`, `spec/gen_spec.rs` and `inputs/q.inputs`. Explore the test
database with `verus_sql`. Write in `tests/`:

* `params.sql`: one parameter tuple per line as SQL literals separated by
  commas (e.g. `'SAUDI ARABIA'` or `DATE '1995-01-01', 24`). Mostly real
  values spread over the domain (large, typical and small answers; for a
  small domain, all of it), plus a few edge values: values that match
  nothing, boundaries, text that differs only in case or spacing, and values
  outside the TPC-H domain (patterns no TPC-H word matches, far dates, finer
  fractions): the kernel must answer those too. Cover each parameter's whole
  TPC-H domain (all colors, nations, ... or a wide spread).
  The pipeline appends random domain tuples to `params.sql` and adds a fresh
  random set to every differential run.
* `txns/NNN.sql`: one write transaction per file (portable SQL; statements
  separated by `;`). They run **in sequence, committed**: each one runs on
  the state the previous ones left, so write each to be valid there (a
  subquery that picks a row must still find one; otherwise it yields NULL).
  * Realistic (most): what the application does, e.g. inserting new orders
    with their lines, deleting orders with their lines, updating the columns
    the query reads (dates, statuses, amounts) on a few rows or a batch;
    transactions that touch several tables; the same kinds of writes in
    different orders and sizes (point, small batch, larger batch); sequences
    that undo or redo earlier writes (insert then delete, update twice).
  * Edge cases (the rest), all within the schema: duplicate rows (the schema
    declares no keys), key changes, rows that enter and leave the answer or
    matter only through EXISTS / NOT EXISTS, changes that need a rescan,
    ties in the ordering, extreme in-range values, text the collation treats
    specially.
  * Every table the query reads gets inserts, deletes and updates. Target
    rows that are in answers: a write that never changes an answer tests
    nothing. Choose rows by SQL (subqueries), not hard-coded ids.
* `setup.sql` (optional): shape the data first, with schema-valid rows only.
* `bench_txns/NNN.sql` (optional): realistic point writes for benchmarking
  (default: txns/).

Run `verus_validate_tests` until every tuple and transaction runs (it applies
the schema's constraints and runs the transactions in sequence).

Feedback from the previous round: {{feedback}}

{{include:shared/rules.md}}
