# Copy-on-write state and the primitive library

Stateful modules keep versioned copy-on-write (COW) state: readers pin a published version, a commit clones only the units it touches, untouched data is shared. State is built from `accelerators/prims/`, the shared library of generic COW structures.

## 1. Reuse

Read the catalogue `accelerators/prims/README.md` ("Which one to use"; it may be empty), then each candidate's header comment (PURPOSE / KEY SPACE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN).

## 2. Otherwise add a generic primitive

Fix the key space, the point-versus-range read shape and the write shape first (a refresh touches about 0.1% of rows with uniformly spread keys, so nearly every unit), then build the container that makes both cheap.

- **Generic:** named for its structure and access pattern, not its user; no table, column, benchmark or query words (comments included).
- **Documented:** header starts with the structured comment above; the catalogue row is generated from it.
- **Tested:** randomized reference tests against a `std::` model: thousands of operations, copy, mutate the copy, check the old version is unchanged.
- **Published:** stage the header and the full updated `accelerators/prims/tests/test_prims.cpp` under `pipeline/prims_staging/<repo-relative path>`, call `primitive_publish(files, rationale)`, fix what it reports until `ok`, then `#include` it. `prims/` is not directly writable.
- **Additions only:** new headers or new lines; never change or remove an existing line, signature or behavior. Duplicates of a published primitive are rejected; prefer a new primitive over modifying one.

A structure only one query needs (e.g. an inverted index for one predicate) stays in the module, still COW and assembled from primitives.

## 3. Never

- Hand-roll a private sharded or chunked COW container in a module.
- Deep-copy state per commit (`make_shared<State>(*old)` over O(rows) members).
- Clone a whole shard or chunk per touched key.

## 4. Maintenance and memory

- A write costs O(changed rows): small or pointer-copied COW units (hot/cold base plus overlay), one copy per touched unit per commit (`maintenance_framework.md`, "Refresh-shaped writes").
- Apply routine inserts, deletes and updates as deltas; rebuild only what a delta cannot express, as a priced `ChooseMaintenance` candidate. Overlays fold into the base incrementally; a delete removes its entry.
- State is compact columnar data, about 10-20 B per row: no node-based map, no per-row heap object. Build by streaming into the final structure without 2-5x transient copies.
