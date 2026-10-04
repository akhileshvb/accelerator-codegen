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
