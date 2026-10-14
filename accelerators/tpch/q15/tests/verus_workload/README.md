# Q15 differential workload

The query takes two DATE parameters, even though workload.json lists only the
start date. Both bounds are supplied explicitly. Text case/spacing coverage is
in supplier payloads rather than the DATE-only parameters.

`params.sql` covers every populated calendar quarter from 1992 through 1998,
a full-history window, rolling quarters, leap day, a one-day historical window,
empty/reversed ranges, and isolated fixture boundaries. The interval is
inclusive at its lower bound and exclusive at its upper bound.

`setup.sql` preserves original rows in helper tables and defines live quarterly
ranking views. It does not change the query's baseline data. All row identities
are selected through SQL; new identifiers are derived from current maxima.

Transactions 001–030 are the main workload: point and batch price/discount
corrections, supplier contact edits, orders with one or eight shipment lines,
quarter-crossing reschedules, cancellation/reinstatement, supplier onboarding,
shipment reassignment, supplier retirement, and registry suspension/reactivation.
Both dependent tables receive inserts, deletes, and updates. Batch corrections
target quarterly winners across the date domain. Changes run in different table
orders and reverse earlier edits in later committed snapshots.

Transactions 031–040 isolate adversarial states outside the historical data:

- 031: exact revenue ties, lower/upper date boundaries, and a zero contribution.
- 032–033: exact supplier/line duplicates, one-copy deletion, same-key payload
  variants, case and significant spaces, and empty versus space-only phones.
- 034–036: supplier-only and line key changes, an orphan global maximum that
  must suppress all joined runners-up, then supplier arrival and UTF-8 text.
- 037–038: zero versus negative totals, discounts above one allowed by the
  schema, negative prices, last-row group removal, and sub-cent revenue.
- 039: nearly cancelling DECIMAL(15,2) endpoints and the minimum INTEGER key.
- 040: delete the complete fixture so previously populated windows become empty.

No CHECK, primary-key, or foreign-key constraints are declared. Every inserted
row clones a complete schema-valid row and replaces selected fields; no nullable
test data is used. Large decimal endpoints multiply by one to keep DuckDB's
intermediate arithmetic representable. The pipeline owns transactions and
concurrent reader snapshots; files contain no explicit BEGIN/COMMIT.
