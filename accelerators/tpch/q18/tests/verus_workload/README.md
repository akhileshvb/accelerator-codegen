# Q18 differential workload

The query takes one parameter, the quantity threshold ($1, HAVING
SUM(l_quantity) > $1). `params.sql` covers the TPC-H domain's ends (312, 315) and
the range 50–351 (including 299/300/301 and 349/350/351 around the setup
orders' totals), plus signed and very large thresholds; then eight seeded draws
from the domain (`scripts/gen_verus_kit_params.py`).

Setup adds eight customers, each with one seven-line order whose quantities sum
to 280, 299, 300, 301, 310, 330, 349 and 350, so every threshold near the
boundary splits them differently. Rows are cloned from existing ones
(`SELECT * REPLACE`) and found again by their comments, never by fixed keys.

Transactions 001–027 are ordinary order-entry work on all three tables the query
reads (customer, orders, lineitem): a new customer and order, quantity
corrections crossing the threshold and back, line insertion and deletion, order
reassignment between customers, renames, batches of new orders, price/date
changes of qualifying orders, and their cleanup. 028–040 are adversarial: equal
totals at the boundary, exact duplicate customers/orders/lines (bag
multiplicity) and deletion of one copy, signed and fractional quantities, key
changes of customers and orders (lines following or orphaned), INT32 minimum
keys, maximal prices, and empty names. `bench_txns/` holds eight point updates
of one setup order for benchmarks.
