# Q8 differential workload

`params.sql` contains 77 tuples: every nation and region, dense LARGE STEEL
and PROMO STEEL answers, sparse observed part types, cross-region supplier
nations, and case/spacing/empty/nonexistent strings. Supplier nationality is
independent of the customer's region; a missing numerator nation still
returns existing year groups with zero market share.

`setup.sql` clones complete schema-valid rows into a small linked fixture.
Keys come from SQL, not assumed TPC-H identifiers. Two customer nations, two
supplier nations, two part types, both years, and unequal discounted line
volumes make dimension and multiplicity changes observable. Original data
remains available, including its dense and sparse groups.

Transactions are committed in filename order:

- 001–017: new orders and lines in both statement orders; point and batch
  amount/discount updates; year moves and reversals; customer/supplier
  nationality and part-type changes; linked inserts and deletes.
- 018–029: nation-region moves, nation/region renaming and restoration,
  linked dimension additions, reassignment, and removal.
- 030: a 64-order line repricing batch combined with order year changes.
- 031–034: exact duplicates in all seven dependencies, fanout through both
  nation aliases, and removal of just one physical copy while another stays.
- 035: coordinated changes to every join-key domain, with a part-type change.
- 036–037: all four inclusive date boundaries, just-outside dates, updating
  a line while its order is excluded, reentry, and line join-key changes.
- 038: simultaneous byte-sensitive nation, region, and part-type changes.
- 039–040: signed DECIMAL(15,2) extrema, discounts of zero, one, and 1.01,
  zero-valued surviving groups, and deletion of a group's final matching part.

The schema places no sign or percentage bounds on decimal columns. After
039, Q8 EDGE has a nonempty 1996 group with zero total volume: DuckDB's
division result must be preserved, not confused with an absent group.
The frozen spec's `defined` predicate requires nonzero group volume, so its
proof domain excludes this schema-valid case. The spec is left unchanged;
this coverage is intentional and exposes the contract mismatch.

No separate benchmark workload is supplied; the pipeline uses `txns/`.
Validation: 77 parameters, 66 initially nonempty, 40 transactions, zero errors.
