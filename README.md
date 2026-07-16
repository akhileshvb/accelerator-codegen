# accelerator-codegen

Prototype host repository for AI-generated DuckDB accelerator infrastructure.

## Build prerequisites

- A sibling DuckDB 1.5.4 checkout at `../accelerator-codegen-duckdb`, or
- `ACCEL_DUCKDB_ROOT=/abs/path/to/accelerator-codegen-duckdb`

The DuckDB checkout must already have a successful Release build that produces:

- `build/release/src/libduckdb.so`, or
- `build/release/src/libduckdb_static.a`

## Configure and build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This initial checkpoint implements only:

- M0 build/link infrastructure
- M1 registration smoke test with `accelerator_hello()`
