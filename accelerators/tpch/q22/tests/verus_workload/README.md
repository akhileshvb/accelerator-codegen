# Q22 differential workload

The original test database has 416 customers and 421 orders; every customer
has an order. Setup retains that history and adds six ordinary unordered
prospects per existing phone prefix, plus small ASCII-only boundary fixtures.
All inserted rows supply every NOT NULL column. Customer and order identifiers
are derived from SQL; duplicate keys and orphan orders are legal in this schema.

The 44 parameter tuples cover every original prefix (10 through 34), the
catalog's seven-code combination, other regional mixes, reordered and repeated
parameters, absent prefixes, significant whitespace, case, empty/short phones,
strict-average equality, fractional-cent averages, and a nonpositive population.
Repeated parameters must act as an IN set, not multiply contributions.

Transactions 001–030 exercise account onboarding, first and subsequent orders,
cancellation, balance and phone corrections, single-account and batch writes,
reactivation, zero-balance transitions, mixed-table commits in both statement
orders, and removal. Batch sizes are 14 and 50 accounts. Transactions 016–020
move the global positive-balance average, including through a customer that
has an order and therefore cannot itself appear in the output.

Transactions 031–040 are the adversarial quarter: exact customer and order
duplicates, removal of one versus the last matching order, key collisions,
order-customer key reassignment, simultaneous rescan and customer changes,
duplicate customer deletion, disappearance and restoration of the positive
population, exact equality, sub-cent averages, maximum/minimum DECIMAL(15,2)
values with a sum exceeding that input precision, and byte-sensitive text moves.
The first/non-last order writes intentionally preserve the output: they must
preserve order multiplicity so that later last-order removals behave correctly.

Files are committed in numerical order. No transaction assumes that an earlier
transaction is rolled back. The pipeline compares all parameter tuples at each
commit and exercises readers retaining earlier snapshots.
