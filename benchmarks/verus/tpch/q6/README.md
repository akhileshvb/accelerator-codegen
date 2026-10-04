# Q6 differential tests

The parameter order is start date, discount center, strict quantity limit,
and exclusive end date. The fourth parameter is intentionally independent:
the SQL and generated specification do not require a one-year interval.

- Annual production-style tuples span every populated year, the usual Q6
  discounts and quantity limits, and both sparse and dense answers. The
  1994 tuples also cover every observed discount value from 0.00 to 0.10.
- Additional tuples exercise fractional quantity boundaries, no matches,
  equal/reversed/narrow date intervals, leap day, signed decimals and the
  maximum DECIMAL(15,2) value. Text parameters do not exist for this query.
- Setup clones a complete schema-valid row into 13 tagged predicate-boundary
  fixtures. It preserves every NOT NULL column rather than relying on defaults.
- Transactions 001–024 are ordinary point/batch corrections, date moves,
  new orders with lines, added lines, mixed updates, undo/redo sequences,
  line deletions and order deletions in both statement orders.
- Transactions 025–032 are the edge quarter: exact bag duplicates, deletion
  of only one duplicate, a key change with a price change, simultaneous
  predicate-boundary crossings, isolated zero contributions, signed
  cancellation, large exact products, strict extreme-quantity boundaries,
  and complete deletion of the scanned table.
- The isolated leap-day slice starts empty, becomes nonempty with zero sum,
  remains zero through cancellation, becomes nonzero, and ends empty again.
  The zero-discount slice also transitions from NULL to zero and back to NULL.
- Keys for inserts are derived from existing data; selected source rows
  survive until their last use. Only lineitem is read by the query; orders
  participate in realistic multi-table application transactions.

The differential pipeline checks every parameter after each committed file
and handles concurrent-reader snapshot checks. No test compares only final
state or assumes that a zero revenue is equivalent to a NULL revenue.
