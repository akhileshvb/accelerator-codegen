# Q20 differential workload

The actual SQL parameter order is start date, nation name, exclusive end date;
workload.json's parameter list is not the query's positional signature.

Setup adds two ordinary suppliers per existing nation, one qualifying product
and stock row each, and five shipments across 1994–1996. New integer identifiers
come from existing maxima; writes select records by SQL, not fixture IDs. The
1995 shipments sum to 40.00 against inventory 21, deliberately near the strict
half-sum threshold. All original data remains in the initial snapshot.

Parameters cover every nation in 1995, observed historical qualifying pairs,
rolling and broad windows, and case/space mismatches, empty and reversed windows,
one-day boundary windows, and dates outside the shipment domain.

Transactions run in numerical order:

- 001–008: point/batch inventory, quantity corrections and year-boundary moves.
- 009–014: product discontinuation/reintroduction and supplier metadata/moves.
- 015–019: stock removal/restocking and shipment replacement.
- 020–024: alternate product/stock/shipments, multiple independent qualifying
  witnesses, depletion and restocking, and alternate-product removal.
- 025–026: new orders and cancellation.
- 027–030: complete vendor/product/order lifecycle across the participating tables.
- 031–033: nation insertion, rename, reassignment and deletion.
- 034–038: large catalog transitions, hidden-neighborhood mutations, and rescans.
- 039–044: duplicate part witnesses, case-sensitive LIKE, duplicate stock/shipment
  multiplicity, and supplier/nation output-bag multiplicity.
- 045–047: strict equality, fractional cents, zero-sum versus missing groups,
  maximum DECIMAL/INTEGER values and signed cancellation (allowed by this schema).
- 048–049: stock-key migration followed by matching catalog/shipment key migration.
- 050–051: ordering ties, UTF-8 names, significant spaces, nation case and exact
  prefix matching. Ties intentionally have different addresses; the generated
  specification permits any order within a tied name and compares output bags.

38 of 51 transactions are ordinary application-style writes; 13 target edge
semantics. The hidden writes in 035 deliberately leave the current answer alone
so later requalification must recover the post-write neighborhood, not stale
build-time data. Duplicate witness operations likewise intentionally preserve
some answers. Snapshot isolation and concurrent readers are exercised by the
pipeline, not by explicit BEGIN/COMMIT statements in these files.

## Parameters now (supersedes the parameter description above)

These tests were written when $2, the p_name LIKE pattern (color || '%') was the literal 'gainsboro%' in the
query. The kit now takes it as a runtime parameter (every TPC-H substitution
parameter is one), so `params.sql` comes from `scripts/gen_verus_kit_params.py`:

1. the curated tuples above, with 'gainsboro%' inserted at its position: each asks
   the question it was written for, and the setup/transactions (built around
   'gainsboro%') keep their meaning;
2. edge values of the new parameter, with the other parameters of the first
   tuple;
3. eight random sets from the TPC-H domain (`eval/compare/gen_tpch_params.py`,
   fixed seed), under the pipeline's random-parameter marker.
