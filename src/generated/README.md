# Generated query tree

Agent-produced query code lives under `src/generated/`. Headers are co-located
with their `.cpp` sources; add `src/generated` to the include path and write:

```cpp
#include "tpch/q16/base/query.hpp"
#include "tpch/q1/aux/module.hpp"
#include "catalog/query_catalog.hpp"
```

## Layout

```
src/generated/
  tpch/q{N}/
    base/
      query.hpp          # bind/kernel API (was tpch_q{N}.hpp)
      fused.hpp          # optional fused-scan API
      single-threaded/   # kernel.cpp, package.cpp, fused_scan.cpp
      parallel/          # kernel.cpp, package.cpp, aligned_agg.hpp
    aux/
      module.hpp, module.cpp
      finalize.hpp, finalize.cpp
  sec_edgar/q{N}/...     # same pattern
  sec_edgar/runtime.hpp, runtime.cpp
  catalog/query_catalog.hpp, query_catalog.cpp
  postgres/kernel_runner.hpp, runtime.hpp, runtime.cpp
```

Hand-written scaffolding remains under `include/accelerator_codegen/`:

- `runtime/` — DuckDB/Postgres extension host, scan adapter, register
- `stateful/` — aux-state substrate
- `harness/` — differential + calibration helpers
