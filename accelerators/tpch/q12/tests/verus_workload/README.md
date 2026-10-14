# Q12 differential workload

The 48 four-argument tuples cover every unordered pair of the seven TPC-H
shipping modes, all dataset years, rolling windows, broad windows, reversed
mode order, repeated modes, empty/reversed windows, exact date boundaries,
case and whitespace variants, nonexistent modes, and extreme supported dates.
The fourth argument is explicit because query/query.sql uses an exclusive end
date; query/workload.json lists only the three original TPC-H arguments.

Transactions 001-024 are ordinary order/line insertion, priority changes,
shipping-mode/date corrections, additional lines, and cancellation. They mix
point writes, 8/12/20-row batches, multi-table writes, both table write orders,
and committed undo/redo cycles. New rows clone complete schema-valid templates;
keys and targets are selected from the current database, not fixture IDs.

Transactions 025-032 cover bag multiplicity on both sides of the join, mixed
priorities at a duplicate key, delete/reinsert rescans, a committed orphan
snapshot after an order-key change and its repair through a line-key change,
strict date inequalities and their equality/reversal cases, byte-sensitive
text, integer key extremes, extreme DATE values, and complete disappearance
of the text-only groups. Extreme keys are intentional schema-boundary values.
Each fixture is tagged in an otherwise unobserved comment column. No setup is
needed, so the build initially exercises the unmodified production-like data.
