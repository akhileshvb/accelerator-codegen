# Accelerators

Everything the pipeline generates or shares for query accelerators.

```
accelerators/
  prims/                 generic, workload-agnostic data-structure primitives (see prims/README.md)
  prooflib/              shared Verus proof library (additions only, via prooflib_publish)
  <bench>/               bench = tpch | sec_edgar
    common/              helpers shared by that bench's queries
    verus_world/         generated world crate (<bench>_world) holding every Verus kit and the one global store
    q<N>/
      cost_model.hpp     where the query keeps its maintenance cost models (every engine)
      layout.hpp         table/column layout the query's state uses
      <engine>/{base,aux}/   per-engine code, including the query's registrars
                         (catalog entry, SQL entry points, runtime binding)
      verus_kit/<crate>/ Verus accelerator: glue, C header, kernel crate in crate/
      config/[verus/]    cost-model JSON (config/verus/ for the Verus kit's models)
      tests/             per-query tests and frozen differential workloads
      bench/             per-query benchmarks and cost-model calibrators
```

`engine_name` is required on accelerator workloads (no default engine). The
shared substrate is aux/txn only, not a portable DataChunk IR. Engine backends
(`backends/<engine>/`) name no query: a query registers itself from its own
directory, so adding one edits no shared file. Include roots are the repository
root and `accelerators/`.

See `pipeline/prompts/shared/generated_layout.md` and the top-level `README.md`.
