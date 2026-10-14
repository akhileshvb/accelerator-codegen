# Q5 differential workload

`params.sql` supplies all three SQL parameters; the explicit end date is read
from `query/query.sql` and `spec/gen_spec.rs`, not the two-entry workload metadata.
It covers all five real regions across 1992–1998, wide and rolling windows,
new region names, exact day boundaries, leap day, empty/reversed windows,
absent names, and byte-distinct case/space variants.

Setup clones complete schema-valid rows and allocates keys from table maxima.
It adds one domestic customer and supplier per nation, four dated orders per
customer, and two ordinary priced/discounted lines per order. This supplements
the sparse test database with answer-producing targets in every real region.
All transactions are sequential; no explicit BEGIN/COMMIT is embedded.

Transactions 001–032 are realistic (73%): point/batch order creation and removal,
price/discount/date corrections, customer/supplier relocation and reversal,
line supplier and order customer corrections, mixed four-table writes, bulk
repricing, territory creation/rename/removal, and region reorganization.
Transaction 017 inserts a line before its order; 019 changes both neighborhoods
in one commit. All six query tables receive inserts, updates, and deletes.

Transactions 033–044 are edges (27%): simultaneous bag duplicates across all
six joins and staged deletion, schema-permitted dangling negative keys followed
by reference repair, inclusive/exclusive date boundaries and leap dates,
duplicate nation keys producing exact revenue ties, nonempty zero-sum groups,
maximum-magnitude DECIMAL(15,2) values (without overflowing query arithmetic),
byte-significant region/group names, merging and splitting aggregate groups.
No key or foreign-key uniqueness is assumed: the schema declares neither.
Targets use SQL selection and fixture labels, not hard-coded identifiers.

`bench_txns/001.sql`–`008.sql` provide a separate realistic point-write cycle:
price/discount changes, date corrections, an order with a line, a mixed update,
deletion, and repricing. Row IDs select a single physical row, including removal
of just one exact duplicate in transaction 034; their values are never hard-coded.
