# Generic data-structure primitives (`accelerators/prims/`)

A growing, workload-agnostic library, empty at the start of a regeneration.
Accelerator modules build their versioned state from these instead of
hand-rolling copy-on-write containers; when none fits, the pipeline adds a
generic one here (additions only, with tests) through `primitive_publish`
rather than writing a private copy inside a module.

Every header starts with a comment giving purpose, complexity, memory per
element and COW/thread rules. Unit and randomized reference tests:
`accelerators/prims/tests/test_prims.cpp`. "COW" means: copying is cheap
(pointer copies), copies share payload, and a write clones only the touched
chunk/shard; mutate a private copy and publish it, concurrent const reads of a
published copy are safe.

### Catalogue

| Header | Primitive | Key | Value | Memory per element | Notes |
| --- | --- | --- | --- | --- | --- |

### Which one to use

No primitives yet.
