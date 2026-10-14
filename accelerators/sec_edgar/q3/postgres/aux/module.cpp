// SEC-EDGAR Q3 on PostgreSQL: compiles the shared, engine-neutral module
// implementation (the build globs <engine>/aux/*.cpp per engine, so exactly one
// copy is ever linked).
#include "sec_edgar/q3/duckdb/aux/module.cpp"
