# Q14 differential workload

The suite has 34 date pairs: 24 ordinary months across 1992–1998, two
larger reporting windows, and eight empty, reversed, single-day, or boundary
windows. Both query parameters are supplied explicitly, including leap day
and the inclusive-start/exclusive-end September boundaries.

The sampled database has 1,777 parts and 1,809 lines. Joined monthly counts
range from one in March 1992 to 184 in June 1995; September 1995 has 58.
Setup retains that data and adds two catalog entries and eight controlled
lines. Cloning complete source rows preserves every NOT NULL column.
Fresh keys and target keys are selected from the live database, not baked in.

Transactions 001–024 are ordinary point and batch price, discount, shipment,
catalog, and part-assignment changes, with new-order insertion/deletion and
multi-table catalog/line lifecycles. Batch sizes vary from 3 to 24 lines.
Transactions run in sequence; fixture labels survive until their last use.

Transactions 025–032 exercise exact duplicate bags, mixed promotional and
non-promotional parts sharing a key, selective duplicate removal, byte/case
and whitespace-sensitive LIKE prefixes, key changes, removal of the last
matching catalog entry, writes to orphan lines, restoration of old and new
orphan lines in the same transaction, maximum DECIMAL(15,2) prices, negative
prices, cent-level products, zero revenue, and discount one. All are permitted
by the schema, which declares neither keys nor value-domain CHECK clauses.

The 2001 window progresses from empty to a single joined line in 030, then
to a nonempty zero-denominator aggregate in 032. The SQL query defines that
last result through DuckDB's division semantics. The frozen spec's `row_ok`
instead requires nonzero revenue for every nonempty aggregate, so it does
not specify this schema-valid SQL state. The test deliberately preserves
the query's behavior rather than imposing an undeclared data restriction.

Every transaction changes a tested answer; 027 includes a contributing line
update alongside an orphan update. The zero-only state occurs only after
the other edge cases. Historical data is retained for snapshot readers while
committed transactions repeatedly alter numerator, denominator, and join
multiplicity.
