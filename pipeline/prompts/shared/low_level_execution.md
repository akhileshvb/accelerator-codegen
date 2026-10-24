# Low-level execution guide

For every inner loop, in stateless kernels and stateful modules. The profile says which loop matters; this says how to make it run at machine speed. Correctness first: a vectorized loop gives the same result as the scalar one.

## Order of work

1. `run_profile`: hot functions, hardware counters (IPC, cache and branch misses), visited-state count. Never guess the hot spot.
2. Attack the top hot spot with the cheapest change that removes its cost: SIMD, branch-free predicates, columnar layouts, prefetch, no allocation or indirection in the loop.
3. Re-measure; repeat until the profile is flat or the loop is at its memory-bandwidth bound.
4. If the read visits too much state or the layout defeats SIMD, loop tuning cannot fix it: restructure the state (stage prompt, structural redesigns), then profile again.

## Vectorize

- AVX-512 / AVX2 intrinsics behind `#if defined(__AVX512F__)` / `__AVX2__`, with a scalar fallback that is the reference. Builds use `-march=native`, so compile-time dispatch is the default; runtime dispatch (`__builtin_cpu_supports`) only where the build target lacks the flag.
- Test each vector path against its scalar path: random inputs, lengths from 0 to a few lanes past a block boundary, NULL and off-range values.
- Check what the compiler vectorized (`-fopt-info-vec`, `perf annotate`); hand-write intrinsics only for loops it does not vectorize.
- Wide lanes never narrow semantics: widen before multiplying, accumulate decimals in 128 bits, no float reassociation where the result must be exact.

## Layout and memory

- Typed columnar arrays (structure of arrays) in cache blocks; a block's working set stays in L2.
- Group by dense ids assigned at build time (intern strings once) and index vectors by id; no per-row string-keyed hash map.
- Fuse filter, probe and aggregate into one pass; materialize nothing a later step re-reads. Predicates are branch-free: compare to a mask, compress to a selection vector, test the most selective first.
- No allocation, virtual call, `std::string` or `shared_ptr` per row; reserve once, reuse buffers across blocks.
- Prefetch irregular accesses (`__builtin_prefetch`, batched probes such as `FindBatch`): issue a block of lookups, then consume.
- Skip columns and blocks that statistics or state already rule out.

## Verify

`perf stat` / `perf record` on the bench binary: IPC, cache and branch misses, time per phase. A change counts when the benchmark moves. At SF3000 a scan is memory-bandwidth bound: judge it by bytes touched per row and rows per second per core.
