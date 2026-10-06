# Scale principles (guidance, not gates)

Iteration runs at SF1; production runs reach SF3000 (TPC-H: lineitem ≈ 18
billion rows, orders ≈ 4.5 billion). Aux state lives in memory. Design so the
accelerator keeps its advantage as data grows:

* **State size**: at most linear, and in what queries read (groups,
  qualifying keys), not in whole tables. At SF3000 it may exceed RAM: the
  store will spill cold state to NVMe, so keep it compact and local (a read
  or write touches few pages, in key order) rather than capping its size.
* **Memory footprint** is the third objective, after fast reads and cheap
  maintenance: never give up a read or per-write bound below for it, but
  among designs that meet those, take the smallest, and size every
  structure. Every accelerated query of the database shares one host with
  the DBMS, so budget about **3 GB of state per accelerator at SF20**
  (TPC-H orders = 30M rows, lineitem = 120M), and a build peak of at most
  about twice that on top of the DBMS's own scans. A state keyed by order or
  customer fits only at about 32-64 bytes per key, all overheads included.
  Count real bytes, not payload (64-bit Rust, glibc malloc):
  * every heap allocation (`Box`, `Arc`, a `Vec`'s buffer) adds about 16
    bytes of allocator header, and an `Arc` adds 16 bytes of counts;
  * a `Vec` is 24 bytes plus its buffer, which grows by doubling (up to 2x
    slack) and holds at least 4 elements once anything is pushed;
  * a `HashMap` is about 1.15-2x its entries' size plus a control byte each,
    and even a one-entry map allocates a table of several slots
    (about 100+ bytes);
  * a persistent-map (`Index`) entry is an `Arc`'d node plus key (plus an
    `Arc`'d value): about 100-150 bytes before the value itself;
  * an `i128` field forces 16-byte alignment: `{i64, i32, i128, i128}` is 48
    bytes, `{i64, i32, i64, i64}` is 32.
  A per-key `Vec` + `HashMap` pair, or a map entry per key of a big table,
  costs 300-500 bytes per key: 10-15 GB per 30M keys. Instead:
  * store **plain fixed-size records** (`Copy` structs, no pointers) in
    **sorted pages of about 64-128 records** under a persistent directory
    (one map node per page, not per record): a lookup is a directory walk
    plus a binary search; a write copies one page and the directory path,
    so versions stay persistent and cheap;
  * spread sparse keys over pages or chunks with a fixed hash or key range,
    so pages stay evenly full;
  * **store narrow, compute wide**: keep sums and counts in the narrowest
    width that holds their SF3000 magnitude (`i64` sums, `u32` counts are
    usually ample), do arithmetic in `i128`, and decline the update (host
    falls back) if a stored value would overflow;
  * keep one copy of each fact: rank or secondary indexes hold small
    records (key + ranking value), not pointers to or copies of full rows.
  State the bytes per entry and the SF20 total (steady and build peak) in
  the analysis, from these costs, not from the payload.
* **Any parameter value**: every substitution parameter is a runtime `$n`
  parameter (LIKE patterns and fractions too) and the state must serve any
  value of its TPC-H domain at the same cost bounds. State kept for one
  parameter value (one color, one nation, one pattern) is not allowed;
  benchmarks and tests draw random values from the domain on every run. A
  decline is allowed only outside the domain and counts against the kit.
* **Maintenance**: cost per changed row, independent of table size. A point
  write touches its own keys; rescans are restricted to touched keys; no full
  rebuild on ordinary writes. A keyed rescan is a scan filtered by key: it
  reads only the touched rows when the table is stored (clustered) or
  indexed by that key (TPC-H `orders` and `lineitem` by orderkey, `part` by
  partkey, `customer` by custkey) and the WHOLE table otherwise (e.g.
  `lineitem` by partkey or suppkey, `orders` by custkey). The frequent
  writes are inserts and deletes of orders with their lineitems (TPC-H
  refresh functions): keep their rescans on clustered keys. An apply that
  declines a valid write makes the host rebuild from scratch: hours at
  SF3000.
* **Reads**: cost in proportion to the answer, not to the state or to the
  rows behind the answer. A read that visits a large slice to return a few
  rows, or that sorts a large result, gives back little of the base query's
  cost. Shape the state so the read path is short:
  * **ranked or thresholded output** (ORDER BY ... LIMIT, MAX / MIN, HAVING
    against a total): keep an index ordered by the ranking value (e.g. groups
    by value within each parameter's slice), so a read walks from the top and
    stops at the limit or threshold;
  * **ordered output**: keep groups in the query's output order, so a read
    emits (or merges a few pre-ordered lists) instead of sorting;
  * **range parameters** (date windows and other intervals): keep coarse
    buckets (e.g. by month or by power-of-two spans) beside the fine ones,
    so a range costs a few coarse lookups plus its two partial ends, not
    one lookup per fine bucket;
  * when the answer itself is large, a read is bounded by it: then make each
    emitted row cheap (no per-row recomputation, no re-sorting).
  Where no parameter-independent state can avoid visiting many keys per read
  (e.g. a ranking whose value depends on a parameter), say so, and still
  confine the visit to keys that can contribute: prune with stored bounds
  (e.g. skip a range whose maximum shows none of its keys can qualify, stop
  a top-k walk once the k-th answer beats the best bound left). State the
  keys visited per read at SF3000: a single-threaded read that visits more
  than a small fraction of the rows the base query scans in parallel does
  not beat it.
* **Production scale**: judge every cost at the production scale above, not
  at the iteration scale: a per-read or per-write term that grows with a
  table is what decides whether the accelerator still wins there.
* **The store**: one Verus store for the whole database (one reader-writer
  lock; every accelerator a slot; a commit is one version of all of them).
  Maintenance (build / apply) runs outside the lock, but every accelerator's
  commit and every read pass through it: keep reads cheap and never hold work
  that could be done before the commit.
* **Versions**: the store keeps a version per commit while snapshots need it,
  so a write must not modify the version readers hold. Use a **persistent**
  structure: a write copies only the path to the keys it changes (e.g. a
  path-copying trie), so a new version costs about its changes. A `vstd`
  `Vec` / `HashMap` behind an `Arc` is not persistent -- updating it clones
  the whole collection, i.e. O(state) per write. (A base plus an overlay is
  persistent only with compaction that keeps the overlay small and lookups
  bounded.)
* **Proof cost**: the kernel is proved, so do not write a new persistent
  structure from scratch: use the shared verified one in the proof library,
  or lift one proven in another query's kernel into the library (see the
  kernel prompts' "Proven code in this tree"). Never trade the per-write or
  per-read cost bounds above for an easier proof: a kernel that copies its
  state per write, or scans all of it per read, is correct but useless.
* **Build**: one pass, or close to it, over the scanned rows, with O(1)
  map operations per row (aggregate per key first, then fan out to coarse
  levels once per key, not once per row). The build is single-threaded:
  at 1 microsecond per lineitem row it already takes 5 hours at SF3000.
  Keep its peak memory near the final state: pre-size containers from the
  input counts, fill pages in place and wrap them into the final structure
  without a second copy (never keep a source map alive while re-bucketing
  it into a new one), process the largest table in bounded chunks so
  temporaries stay small, and drop each temporary as soon as it is consumed.
* **Magnitudes**: keys and aggregates must not overflow at SF3000 (order keys
  exceed 2^31; sums of decimal products can exceed i64: use i128). Overflow is
  safe (the kernel declines) but a decline at scale is a performance cliff.
* **No SF1 constants**: size from the inputs, never from literals.
* **Final answers**: do not cache the query's final answer as the state,
  unless the answer is small, keeping it right is really cheap (a changed row
  updates it locally, no recount), and it gives a large speedup for the real
  parameter distribution. Justify it in the analysis if you do.
