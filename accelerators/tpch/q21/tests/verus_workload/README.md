# Q21 differential workload

The query takes one parameter, the nation name ($1). `params.sql` lists seven
TPC-H nations, the setup's adversarial nations (empty, whitespace, case and
non-ASCII variants, a duplicated key, a negative key, one with more than the
LIMIT of 100 answer groups), near misses ('ALGERIA ', ' SAUDI ARABIA'), an
absent nation and NULL (the kit declines NULL: those executes fall back to SQL),
then eight seeded draws from the domain (`scripts/gen_verus_kit_params.py`).

Setup adds those nations with suppliers, finished and open orders, and lines
whose receipt/commit dates make a supplier the only late one, one of several
late ones, or exactly on time; duplicate primary lines (bag multiplicity),
same-name suppliers, suppliers and nations that arrive in later transactions,
and 120 single-supplier groups for one nation. Every column is non-NULL
(the declared schema's NOT NULL constraints apply before setup).

Transactions 001–022 move rows across every predicate of the query on all four
tables it reads (supplier, lineitem, orders, nation): order status flips, late
and on-time receipt changes, added and deleted peer lines, order deletion,
supplier and nation renames and key changes, duplicate supplier keys, late
arrival of a missing nation, and order key changes. 023–030 are ordinary order
processing: a finished multi-supplier order with one late supplier, a second
late delivery and its correction, cancelling and re-adding peer lines, a batch
of orders finishing, a supplier relocating to SAUDI ARABIA, and archiving the
order. `bench_txns/` holds four status/name updates for benchmarks.
