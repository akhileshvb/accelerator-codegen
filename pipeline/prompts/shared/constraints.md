# Stage boundaries (stateless kernels)

**This stage owns** the query's own files under `accelerators/{benchmark}/q{N}/{engine}/`: kernel, package, header, tests, benchmark, registrar.

**It must not touch** `backends/`, `substrate/`, `cmake/`, the root `CMakeLists.txt`, other queries, or `accelerators/prims/` (the primitive library grows only through `primitive_publish`, and stateless kernels have no persistent state to put in it). A missing backend helper goes to the request log in `query_host_bootstrap.md` (Ownership).

**Rules**

- `engine_name` is required; never default to an engine.
- Base tables come from this engine's native scan APIs. A stateless kernel keeps no state across executions: transient state is rebuilt per execution.
- Keep generic fallbacks for unexpected layouts, NULLs and transaction-local rows. Do not change query semantics for convenience.
- Reads and builds split data-proportional work over the backend's one thread budget through `services` (`engine_interfaces.md`, `substrate_primitives.md`); a serial loop over the data is a defect at SF3000.
- Benchmark against the engine's own path at an equal core budget (same `taskset`, same thread count).

**Judged by** the gates: build, correctness (differential vs the reference SQL), baseline speedup, profile, fairness, audit.
