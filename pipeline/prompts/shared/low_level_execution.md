# Low-level execution guide

For every inner loop, in stateless kernels and in stateful modules. The profile says which loop matters; this guide says how to make it run at machine speed. Correctness comes first: a vectorized loop must give the same result as the scalar one.

## Vectorize

- Process columns with SIMD: AVX-512 and AVX2 intrinsics behind `#if defined(__AVX512F__)` / `__AVX2__`, and a scalar fallback that is the reference. Builds use `-march=native`, so compile-time dispatch is the default; use runtime dispatch (`__builtin_cpu_supports`) only where the build target lacks the flag.
- Test every vector path against its scalar path: random inputs, lengths from 0 to a few lanes past a block boundary, NULL and off-range values.
- Check that the compiler vectorized what you expect (`-fopt-info-vec`, `perf annotate`); hand-write intrinsics only for loops it does not vectorize.
- Wide lanes must not narrow semantics: widen before multiplying, accumulate decimals in 128 bits, no float reassociation where the result must be exact.

## Layout and passes

- Typed columnar arrays (structure of arrays) sized in cache blocks; the working set of a block stays in L2.
- Group by dense ids assigned at build time (intern strings once), then index vectors by id; no hash map keyed by string per row.
- Fuse filter, probe and aggregate into one pass over the data. Materialize no intermediate that a later step re-reads.
- Predicates are branch-free: compare to a mask, compress into a selection vector, test the most selective one first.

## Memory

- No allocation, virtual call, `std::string` or `shared_ptr` per row; reserve once, reuse buffers across blocks.
- Prefetch irregular accesses (`__builtin_prefetch`, batched probes such as `FindBatch`): issue a block of lookups, then consume them.
- Read what the query needs: skip columns and blocks that statistics or the state already rule out.

## Verify with hardware counters

Run `perf stat` and `perf record` on the bench binary: IPC, cache and branch misses, time per phase. A change counts when the benchmark moves, not when the code looks faster. At SF3000 a scan is memory-bandwidth bound: judge it by bytes touched per row and rows per second per core.
