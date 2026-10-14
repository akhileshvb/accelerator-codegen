# Q7 differential test workload

All files are committed sequentially; do not reorder them. Setup adds complete,
schema-valid customer/supplier/order/line fixtures identified by `q7` comments.
Identifiers are selected from data or allocated using the current maximum.
No helper tables, persistent temporary tables, or external data are required.

## Parameters

`params.sql` covers all 325 unordered pairs (including the 25 equal-name pairs)
of the 25 TPC-H nations. Q7 is symmetric in its two parameters. Eight additional
reversed pairs check that symmetry explicitly. The remaining tuples exercise
new territory names, zero-revenue groups, missing names, byte-sensitive case,
leading/trailing whitespace, empty text, Unicode, embedded quotes, and lengths.
All six text aliases become populated in transaction 044, so their parameters
are not merely permanent empty-answer checks.

## Transactions

- 001-007: insert order/lines; point price, discount, date and customer changes.
- 008-011: supplier/customer geography updates and undo sequences.
- 012-015: create a two-order trading chain, update several tables, delete one
  order with its lines, then remove dimensions while the second order remains.
- 016-020: insert 12 orders with 36 lines; small and larger batch changes;
  selectively delete four orders with their lines.
- 021-023: change 64 existing answer lines and eight existing suppliers/customers.
- 024-026: introduce, rename and remove a territory, changing both join sides.
- 027-032: first and last qualifying lines enter/leave an order's EXISTS input;
  inclusive endpoints, immediately adjacent dates, leap day, then order-first deletion.
- 033-036: delete/recreate an order, remove the remaining batch, mixed point writes.
- 037-038: exact duplicates in every query table, including both nation join roles;
  delete only one occurrence using rowid, preserving original multiplicities.
- 039: coordinated join-key changes in all five tables, plus a visible cent change.
- 040-041: missing supplier/customer/nation rows and dimension-first reconstruction.
- 042-043: line-before-order insertion, then restore the join and EXISTS membership.
- 044-048: byte-distinct nation aliases sharing a key; an isolated zero-revenue group;
  maximum-magnitude signed prices that cancel; zero-valued duplicate contributions;
  discount 1 and maximum representable discount on zero price; deletion to negative,
  back to zero (the group must persist), and finally empty (the group must disappear).

The first 36 transactions are ordinary point/batch business writes (75%); the
last 12 are schema-valid adversarial writes (25%). Every table read by Q7 has
inserts, updates and deletes. All NOT NULL fields are supplied. No uniqueness,
foreign-key, price-sign or discount-range constraints are invented: the supplied
schema declares none. Extremes fit DECIMAL(15,2); arithmetic remains in range.
Transaction 042 also adjusts an existing answer line so an otherwise unjoined
insert still produces an observable answer change at its committed snapshot.
Transaction 045 likewise adjusts an existing answer line while testing exact
cancellation within the separately observable zero-revenue group.

`bench_txns/` contains six repeatable point transactions: amount, discount,
year, order customer, supplier geography and customer geography changes.
These use the setup fixtures and avoid timing artificial duplicate/extreme
workloads. They are valid both immediately after setup and after transaction 048.
