#pragma once

// SEC-EDGAR Q3 module for PostgreSQL. The state, kernels (AVX2 with runtime
// dispatch and scalar fallback) and incremental maintenance are engine neutral
// (they only see core::AuxRow / InitialScan), so the PostgreSQL build compiles
// and uses the very same module as DuckDB instead of a forked copy.
#include "sec_edgar/q3/duckdb/aux/module.hpp"
