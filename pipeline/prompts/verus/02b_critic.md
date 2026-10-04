# Verus pipeline: critique the analysis ({{benchmark}} q{{query}})

Decide: **will this design be fast at production scale (SF3000) for reads
AND writes, and provable?** The query, the spec, the analysis and the
declared inputs are inlined below. A design you accept is built and proved
over hours; one that breaks at scale wastes all of it. Reject it now.

**First, estimate at SF3000** (`scale_principles` gives the table sizes),
from the design itself, not from the analysis's own numbers (they have
undercounted per-entry cost by 5-20x):

* **state**: entries per structure x real bytes per entry (60-100 bytes of
  map overhead plus the value), summed, at SF20 and at SF3000;
* **read**: keys visited by one read, for the worst parameter values,
  next to the answer's size and the rows the base query scans;
* **write**: work per changed row, for each table, including the physical
  cost of each rescan (a rescan on a key the table is not clustered or
  indexed by reads the whole table);
* **build**: map operations per scanned row.

Then go through these checks, in order. A check fails when you can name a
concrete failing case (a specific write, parameter value or data size and
what goes wrong) or when the analysis leaves the cost unbounded and your
estimate breaks the check.

* **C1 writes**: every table the query reads is maintained under insert,
  delete and update, including key changes and duplicate rows (tables have no
  declared keys).
* **C2 cost of a write**: no ordinary write forces a rebuild or a rescan of a
  whole table. Fails if a frequent write (inserts and deletes of the fact
  tables' rows, e.g. orders with their lineitems) needs a rescan keyed on a
  column the table is not clustered or indexed by, if any write's cost grows
  with a table, or if apply can decline a valid write (a decline rebuilds
  from scratch).
* **C3 size**: the state grows with what queries read, not with whole tables;
  a read does not touch the whole state. Fails if your SF20 state estimate
  does not fit comfortably in RAM (more than ~50 GB), or the state keeps a
  map entry per row of the largest table. At SF3000 the state may spill to
  NVMe: there it fails only if a read or a write would touch many scattered
  entries (no locality), not for its size alone.
* **C4 values**: no overflow at SF3000; NULLs handled; every parameter value
  can be served.
* **C5 no answer cache**: the state is not a cache of final answers (unless
  the analysis justifies it convincingly).
* **C7 fast reads at scale**: the read path's cost follows the answer, not
  the state. Fails if a read visits a slice that grows with SF while the
  answer does not (ORDER BY ... LIMIT, MAX / MIN, a threshold), sorts a large
  result it could keep pre-ordered, or probes one fine bucket per unit of a
  range parameter. When the ranking value depends on a parameter, the
  analysis must still confine the walk to keys that can contribute (stored
  bounds to skip ranges, an early stop for top-k) and state the keys
  visited; "it must visit them" without that fails. A read visiting more
  than ~1% of the rows the base query scans fails: the base scan is
  parallel, the read is not.
* **C6 provable and persistent**: every structure is a map (or set) keyed by
  tuples whose entries are sums or counts over the database, provable with
  `contracts/proof_method.md`'s patterns, AND persistent: the analysis names
  a structure where a write copies only the path to the keys it changes (the
  shared verified one in the proof library, or one proven in another query's
  kernel). It fails if the state would be a `vstd` `HashMap` / `Vec` cloned
  per write, a flat list searched linearly, or an overlay without
  compaction that bounds its depth; or if a new custom encoding (prefix
  arrays, multi-level buckets, compression, packed offsets) comes without
  the cost bound it is needed for and its invariant and proof plan.
* **C8 build**: one pass with O(1) map operations per scanned row. Fails if
  the build does per-row work over many levels or keys (fan out per key
  after aggregating, not per row) or keeps a temporary per row of the
  largest table beyond the state.

Judge from the material below: do not explore the workspace. Use a tool
only if one specific claim decides a check (a few calls at most:
`verus_check_inputs`, `verus_sql`). Then write `analysis/critique.json` and
stop:

`{"estimates": {"state": "...", "read": "...", "write": "...", "build": "..."}, "checks": [{"id": "C1", "ok": true}, ..., {"id": "C7", "ok": false, "case": "...", "suggestion": "..."}], "verdict": "accept" | "revise"}`

`verdict` is `revise` if any check fails. Each estimate is one line with
its numbers; a failing check is one or two sentences each for `case` and
`suggestion`, and the suggestion names the structure or key change that
fixes it. Style, naming and nice-to-haves are not failures.

## Material

You have these files in full below: do not re-read them.

{{file:query/query.sql}}

{{file:analysis/analysis.json}}

{{file:inputs/q.inputs}}

{{file:contracts/scale_principles.md}}

{{file:spec/gen_spec.rs}}

{{include:shared/rules.md}}
