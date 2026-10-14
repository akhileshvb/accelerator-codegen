# Q13 differential tests

`query/query.sql`, `spec/gen_spec.rs` and `inputs/q.inputs` define a
parameter-free query with the literal `%unusual%accounts%`. The parameter
listed in `query/workload.json` is inconsistent with that contract. An empty
`params.sql` uses the validator's single implicit zero-argument tuple; there
are no pattern parameters to vary.

Setup preserves the sampled workload and adds eight ordinary customers with
0, 1, 2, 3, 5, 8, 12 and 20 qualifying orders, plus excluded orders. This
exercises zero counts, distinct histogram buckets and secondary ordering ties
at build time. All generated identifiers derive from current SQL maxima.

Transactions 001–024 model point and batch order placement, cancellations,
comment corrections, reassignments, customer onboarding, account consolidation
and removal. They include both table-update orders, reversals, and writes to
the original sampled data. Transactions 025–032 cover bag multiplicities,
single-copy deletion, key collisions, orphan orders, INTEGER boundaries,
case-sensitive and ordered substring matching, all-excluded orders, empty
orders, empty customers, and restoration. The sequence deliberately retains
orphans and duplicate keys: neither keys nor foreign keys are declared.

The last four transactions create test-only backups, exclude every order,
remove each relation, then restore it. Removing orders also removes one
zero-count customer so this transaction changes the histogram. Empty states are committed separately
so differential and concurrent-snapshot checks can observe them. No schema
constraints are relaxed, and every base-table insertion supplies all columns.

`bench_txns/` separately exercises an ordinary point order insertion, two
comment corrections across the predicate boundary, and cancellation, without
benchmarking the adversarial full-table removals.
