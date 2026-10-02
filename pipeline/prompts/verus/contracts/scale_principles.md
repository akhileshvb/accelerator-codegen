# Scale principles (guidance, not gates)

Iteration runs at SF1; production runs reach SF3000 (TPC-H: lineitem ≈ 18
billion rows, orders ≈ 4.5 billion). Aux state lives in memory. Design so the
accelerator keeps its advantage as data grows:

* **State size**: at most linear, and in what queries read (groups,
  qualifying keys), not in whole tables. It must fit in memory at scale.
* **Maintenance**: cost per changed row, independent of table size. A point
  write touches its own keys; rescans are restricted to touched keys; no full
  rebuild on ordinary writes.
* **Reads**: cost in proportion to the slice the query needs (e.g. one
  parameter's groups), not the total state.
* **Versions**: the store keeps a version per commit while snapshots need it.
  Share structure (`Arc`, a base plus a small overlay) so a new version costs
  about its changes, not a copy.
* **Proof cost**: the kernel is proved, so choose structures whose proofs
  stay small: `vstd` collections (`Vec`, `HashMap`) shared through `Arc`
  with a small overlay, rather than a hand-written balanced tree or index
  that must be proved from scratch. A structure's speed only matters if the
  kernel that uses it can be proved.
* **Build**: one pass, or close to it, over the scanned rows.
* **Magnitudes**: keys and aggregates must not overflow at SF3000 (order keys
  exceed 2^31; sums of decimal products can exceed i64: use i128). Overflow is
  safe (the kernel declines) but a decline at scale is a performance cliff.
* **No SF1 constants**: size from the inputs, never from literals.
* **Final answers**: do not cache the query's final answer as the state,
  unless the answer is small, keeping it right is really cheap (a changed row
  updates it locally, no recount), and it gives a large speedup for the real
  parameter distribution. Justify it in the analysis if you do.
