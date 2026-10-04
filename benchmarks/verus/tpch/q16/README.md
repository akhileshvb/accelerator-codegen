# Q16 differential workload

The query takes nine parameters: the excluded brand and eight integer sizes.
The workload metadata still lists a type-pattern parameter, but the SQL and
generated specification fix that pattern to `SMALL BRUSHED%`.

`params.sql` contains 40 tuples: 30 ordinary catalog selections covering every
TPC-H brand and size 1–50, and 10 boundary, repeated/permuted-size, case,
whitespace, Unicode, absent-brand, and empty-answer selections.

`setup.sql` preserves the native data and adds a small catalog with four-source
parts, two initially single-source parts, a complaint-blocked supplier, and
equal-count groups. Keys are allocated relative to the existing database;
subsequent transactions locate fixtures by names, not native IDs. Cloned rows
retain values for every required column.

Transactions run in numeric order without explicit transaction delimiters:

- `001`–`017`: supplier onboarding, part creation, brand/size/type corrections,
  second sourcing, supplier reassignment, complaint opening/resolution, and
  deletion/recreation of a complaint-blocked supplier.
- `018`–`036`: native-data batches of 20 and 128 parts, 24 edge deletions and
  64 edge insertions, multi-table catalog retirement/restoration, mixed writes
  in different orders, last-eligible-source transitions, and contract renewal.
- `037`–`048`: duplicate part/edge multiplicity and partial removal; same-key
  parts with different groups; multiple blocking supplier rows and removal of
  only one blocker; coherent and deferred key migrations; orphan edges later
  joined by a new part; filtered-part rescans; signed integer and decimal
  boundaries; exact LIKE-prefix/case/order semantics; byte-sensitive text and
  tied counts; fixture teardown.

Transactions `038` and `041` intentionally preserve the answer. They check that
removing one duplicate does not remove a still-supported distinct supplier or
release a supplier with another blocking row. The following transactions remove
the last support or blocker, making incorrect multiplicity observable.

No NULL values or key/foreign-key assumptions are used: the declared schema has
NOT NULL columns but does not declare unique or foreign keys. Supplier absence
does not disqualify a partsupp row; only a matching complaint row does.

The pipeline checks all tuples after setup and each committed transaction,
including its cross-connection snapshot checks. No separate local engine runner
is needed.
