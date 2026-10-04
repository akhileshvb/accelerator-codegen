# Q1 differential workload

`params.sql` deliberately interleaves cutoffs: 21 offsets cover the ordinary
60–120 day workload and smaller historical answers; seven cover all rows,
future cutoffs, the oldest row's inclusive boundary, and empty answers.
The original database has 1,809 lines and four groups; offsets 90, 2000,
2484, and 3000 initially select 1,801, 215, one, and zero lines respectively.
Setup adds nine exact/adjacent cutoff fixtures and four byte-distinct groups
so both build-time grouping and date inclusivity are tested.

Transactions run in numeric order, without explicit transaction control:

- 001–009: a three-line order, independent changes to every aggregate input,
  status/return migrations, and a shipdate crossing followed by its undo.
- 010–011: corrections to eight existing, qualifying lines.
- 012–018: a seven-line order, mixed order/line corrections, batch shipdate
  changes and their undo, and line/order deletion and replacement.
- 019–024: six four-line orders, larger mixed updates, partial deletion,
  restoration of earlier values, and cleanup of new orders and their lines.
- 025–029: exact duplicates, deletion of just one copy, key/group changes,
  full-range signed decimals, zero sums with positive counts, zero and full
  discounts, byte ordering (case, empty strings, spaces, UTF-8), and cutoff
  crossings for singleton groups before their deletion.
- 030–032: deletion of an entire canonical group, deletion of the whole table,
  and repopulation of the empty table with old and novel groups.

Thus 24 of 32 transactions are ordinary application writes and eight are
adversarial. All 16 lineitem columns are populated on inserts. Keys come
from SQL selections or maxima, not database-specific IDs. Only lineitem is
a query dependency; orders is also modified for realistic multi-table work.
No keys, positivity checks, or discount/tax business ranges are declared by
the schema, so duplicates and signed decimal values are legal. Extreme
decimal products remain within DuckDB's DECIMAL(38) intermediate range.
Q1 has no EXISTS, NOT EXISTS, or rescan input; its relevant adversarial cases
are aggregate multiplicity, group lifetime, byte ordering, and date cutoffs.

The pipeline supplies committed snapshots and reader isolation checks; these
files do not issue BEGIN/COMMIT or perform their own snapshot management.
