# Copy-on-write state and the primitive library

Stateful modules keep their state as versioned copy-on-write (COW) data: readers pin a published version, a commit clones only the units it touches, and untouched data is shared. The state is built from `accelerators/prims/`, the shared library of generic COW structures. Good primitives are the main lever for fast reads and cheap maintenance.

## 1. Reuse a catalogued primitive when it fits

Read the catalogue in `accelerators/prims/README.md` ("Which one to use"), then the header comment of each candidate (PURPOSE / KEY SPACE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN). The catalogue may be empty.

## 2. Otherwise design a bespoke, generic primitive for this access pattern

Choose the key space, the point-versus-range read shape and the write shape (a refresh touches ~0.1% of rows with keys spread uniformly, so nearly every unit) first. Then build the container that makes both cheap.

- **Generic.** Name it by what it is (`ShardedSortedMap`), not by who uses it. No table, column, benchmark or query words, comments included. Engine-exact value semantics belong to the backend API, never to `prims/`.
- **Documented.** Start the header with the structured comment (PURPOSE / KEY SPACE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN). The catalogue row is generated from it.
- **Tested.** Randomized reference tests against a `std::` model: thousands of operations, copy, mutate the copy, check the old version is unchanged.
- **Published.** Stage the header and the full updated `accelerators/prims/tests/test_prims.cpp` under `pipeline/prims_staging/<repo-relative path>`, then call `primitive_publish(files, rationale)`. Fix what it reports and retry until `ok`. `prims/` is not directly writable. After a publish, `#include` it.
- **Additions only.** New headers, or new lines in an existing header; never change or remove an existing line, signature or behavior. A duplicate of a published primitive is rejected: reuse or extend it. Prefer a new primitive over modifying one.

A structure that only one query needs (an inverted index for one predicate, a query-specific cube layout) stays in the module, but it is still COW and assembled from primitives.

## 3. Never

- Hand-roll a private sharded or chunked COW container inside a module.
- Deep-copy the state per commit (`make_shared<State>(*old)` over O(rows) members, copying all rows to apply a delta).
- Clone a whole shard or chunk per touched key.

## 4. Maintenance and memory budget

- A write costs O(changed rows): the COW unit is small or pointer-copied (hot/cold base plus overlay), so a refresh does not copy whole chunks (`maintenance_framework.md`, "Refresh-shaped writes").
- Apply routine inserts, deletes and updates as deltas. Rebuild only what a delta cannot express, as a priced `ChooseMaintenance` candidate; no hard-coded size guard forces a rebuild. Overlays fold into the base incrementally; a delete removes its entry.
- State is compact columnar data, about 10-20 B per row: no node-based map, no per-row heap object. Build by streaming into the final structure, with no 2-5x transient copies.
