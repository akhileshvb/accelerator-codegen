# Q19 differential workload

The setup adds 12 normal catalog products, one order per product, and 60
shipments. All 12 allowed containers and the inclusive size endpoints are
represented. Quantities include both ends of the three ordinary windows and
one cent just outside each end. Every inserted row clones a complete source
row before replacing fields, preserving all schema-required values.

Parameter tuples cover all 25 ordinary brands, mixed-brand normal windows,
the larger native answers, and isolated empty, case-sensitive, whitespace,
zero, negative-window, and maximum-decimal cases. All six parameters are
supplied on every line; quantity parameters are integer literals.

Transactions 001–030 are the normal workload: native contributing-row edits,
point and batch price/discount/quantity edits, shipping eligibility changes,
catalog changes, restocking, order/line insertions and deletions in both
statement orders, and repeated edits that undo earlier edits. Transactions
031–040 are edge workloads: exact duplicate bag rows, one-copy deletion,
paired and unpaired join-key changes, last qualifying part removal with an
ineligible-period line insert, part deletion and reinsertion over surviving
lines, part-driven rescans, size boundaries, exact
text comparison, zero versus NULL SUM, and maximum DECIMAL(15,2) values.

Selection uses SQL-derived keys or fixture names, never source-data key
constants. Numbered files are committed in order. The zero-only brand moves
from no rows to zero revenue, then an extreme revenue and a discount-one
zero contribution, then back to no rows. The key-change and last-part
sequences deliberately retain existing lines so a part-only change must
invalidate or rediscover the lines in the declared filtered scan.
