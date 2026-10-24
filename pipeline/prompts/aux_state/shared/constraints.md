# Stage boundaries (stateful modules)

**This stage owns** the query's own files: `aux/module.{hpp,cpp}`, `aux/finalize.{hpp,cpp}`, layout, tests, benchmark, calibrator, cost-model config, and the append-only registrations listed in `agent_boundary.md` and `registration_checklist.md`. State is per query (`{{aux_function_name}}`).

**It must not touch** `backends/`, `substrate/`, `cmake/`, the root `CMakeLists.txt`, other queries' files, the query's `base/**` kernel (a separate accelerator), or `accelerators/prims/` except through `primitive_publish` (`cow_primitives.md`).

**Rules**

- Reads, builds and maintenance split data-proportional work over the backend's one thread budget through `services` (`engine_interfaces.md`); the result is identical at any thread cap.
- Fallback is required: when state is missing, stale or transaction-blocked, the engine answers from SQL (the runtime's `Fallback`, the **sql_fallback** role). The module does not depend on a base kernel.
- Invalidation and staged publication are transaction-safe. Irrelevant mutations are NoOp: no epoch bump, no stale mark. Other queries' writes run concurrently (`concurrency_contract.md`).
- Reads are bounded to the query window: no scan of all partitions or groups when the query needs a subset.
- A rebuild selected by the cost model resyncs state for later hits; invalidate-only is not a rebuild.
- Do not change query semantics for convenience. Benchmark the full production path and consume all results.

**Judged by** the gates: `aux_correctness`, `aux_unit`, `prims_tests`, `aux_multi_connection`, `aux_cross_accel`, `aux_tsan`, `aux_bench_sweep`, audit.
