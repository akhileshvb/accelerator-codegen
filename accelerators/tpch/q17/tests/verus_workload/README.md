# Q17 differential workload

Setup preserves the existing data and adds 32 products across 12 ordinary
brand/container pairs, each with five lines of quantities 1, 3, 10, 20, 40.
The initial threshold is 2.96: only quantity 1 contributes. Raising quantity
40 to 50 moves the threshold to 3.36, so an untouched quantity-3 line enters.
All inserted rows copy complete schema-valid templates and allocate keys
using SQL; names/comments identify fixtures, not hard-coded source keys.

The 35 parameter tuples include 27 ordinary pairs, including both contributing
pairs found in the original database, and eight exact-text/no-match edge pairs.

Transactions run in filename order:
- 001–006: point/batch repricing, low-quantity withdrawal/restoration, and
  high-quantity threshold movement/restoration across sixteen products.
- 007–011: point and 24-product shipment insertion, repricing, and deletion.
- 012–016: individual and batch catalog reclassification, followed by undo.
- 017–022: two complete product/line lifecycles, reversing table write order
  and mixing catalog and line changes within a single commit.
- 023–024: remove/reinsert a catalog row while its contributing lines remain.
- 025–026: exact duplicate bags, deletion of one occurrence, and six simultaneous
  same-key catalog variants with case, whitespace, Unicode, and empty text.
- 027: catalog key migration, corresponding line migration, then reparent one
  contributing line into another existing group.
- 028–029: exact threshold equality (1 and 9), then a fractional crossing
  (1 and 9.05) with a nonempty zero-price answer instead of NULL.
- 030–031: signed quantities, cancelling prices, and DECIMAL(15,2) extrema with
  a one-cent residual; arithmetic must preserve both count and price moments.
- 032: remove all fixture lines and catalog variants, including each last
  group member; restore the original dependent-table contents.

Transactions 001–024 are ordinary workload shapes; 025–032 are edge cases.
There are no NULL writes and no assumed uniqueness or foreign-key constraints.
Snapshot checking is performed by the pipeline, not by SQL BEGIN/COMMIT commands.
