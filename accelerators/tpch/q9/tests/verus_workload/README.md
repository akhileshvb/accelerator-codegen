# Q9 differential workload

The frozen query has no parameters and always uses case-sensitive `LIKE
'%green%'`. `workload.json`'s color metadata does not describe the frozen SQL.
The blank `params.sql` requests the validator's default empty tuple.

Setup snapshots one fully joined, answer-producing source row and derives
unused fixture keys from the database maxima. It does not modify the six
query tables. Snapshots supply every required column when cloning rows.

Transactions 001–036 are ordinary catalog, supplier-cost, geography, order,
and line maintenance: point and batch updates, mixed-table writes, new
orders, cancellations, redo, and catalog eligibility changes. They include
both fact-first and dimension-first insertion and coordinated removal of
dependent rows during supplier, catalog, geography, and order cancellations.
Transactions 037–048 cover bag multiplicity, unequal duplicates, coordinated
key changes, missing-order backfill, eligibility/date-cache rescans, byte
text grouping, zero-profit live groups, large exact decimals, and final
group disappearance. All six query tables receive inserts, updates, and
deletes on participating rows. Deletes and restorations are sequenced.

Fixture IDs are selected through SQL, never copied from observed IDs. The
original answer-producing seed remains available throughout the sequence.
Temporary orphan relationships are schema-valid: this schema declares no
foreign keys or uniqueness constraints. Duplicates are intentional.

## Parameters now (supersedes the parameter description above)

These tests were written when $1, the p_name LIKE pattern ('%' || color || '%') was the literal '%green%' in the
query. The kit now takes it as a runtime parameter (every TPC-H substitution
parameter is one), so `params.sql` comes from `scripts/gen_verus_kit_params.py`:

1. the curated tuples above, with '%green%' inserted at its position: each asks
   the question it was written for, and the setup/transactions (built around
   '%green%') keep their meaning;
2. edge values of the new parameter, with the other parameters of the first
   tuple;
3. eight random sets from the TPC-H domain (`eval/compare/gen_tpch_params.py`,
   fixed seed), under the pipeline's random-parameter marker.
