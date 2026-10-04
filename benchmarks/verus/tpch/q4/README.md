# Q4 differential workload

The query and frozen spec take two independent DATE parameters, even though
workload.json lists only start_date. params.sql tests every populated quarter,
monthly and point windows, a wide historical range, inclusive/exclusive
boundaries, empty/reversed ranges, missing dates, and wide valid dates.
Text is not a query parameter; case, spaces, empty strings, and UTF-8 are
exercised through order priorities in transaction 034 and updated in 035.

Setup adds 20 normal orders with one qualifying and one equal-date line each.
All columns are copied from complete schema-valid templates; query columns and
test markers are replaced explicitly. Generated order keys come from SQL;
only the integer endpoint test uses literal keys. The original data stays in
place and transactions 024-026 also modify qualifying original rows.

Transactions 001-027 are realistic order/line insertion, cancellation, date
correction, priority correction, and shipping-date workloads. Sizes range from
one order to 24 orders/72 lines. Both table-write orders are covered. Date and
priority corrections are undone and redone; both shipping-date columns cross
the strict inequality; final-witness removal and restoration are committed
separately. Transactions 018-019 change both tables at the same keys.

Transactions 028-036 are the nine edge cases (25%): identical duplicate orders,
same-key orders with different dates/priorities, duplicate witnesses, loss and
restoration of the last shared witness, order and line key reassignment,
orphan lines, witness replacement with zero net qualifying-key delta,
case/space/UTF-8 byte ordering, extreme INTEGER keys and DATE values, and
cleanup of entire uncommon priority groups. In 029 a new order is inserted
after its shared key loses all witnesses; 030 must discover every surviving
order variant. In 033 the witness is replaced at one key while the order moves
date and priority, exercising simultaneous zero-net line and nonzero order
captures. Same-priority orders are aggregation ties, not extra result rows.

Files run in numeric order as committed transactions. The pipeline compares
all parameter tuples after setup and each commit and handles concurrent reader
snapshots; transaction files deliberately contain only writes, no explicit
transaction-control statements. Tests do not assume catalog keys or foreign
keys absent from the schema.
