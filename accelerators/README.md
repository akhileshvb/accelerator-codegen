# Generated sources

Query accelerators live under **per-engine** trees:

```
src/generated/{tpch|sec_edgar}/q{N}/{engine}/...
src/generated/catalog/{engine}/query_catalog.{hpp,cpp}
src/generated/{engine}/   # host glue from engine_backend (e.g. postgres_extension.cpp)
```

`engine_name` is required on accelerator workloads (no duckdb/postgres default).
Shared substrate is aux/txn only — not a portable DataChunk IR.

See `pipeline/prompts/shared/generated_layout.md` and
`pipeline/docs/per_engine_accelerators_smoke.md`.
