// GENERATED QUERY: registers this query's DuckDB base kernel table function.
// The backend (backends/duckdb/api/register.cpp) registers every registered
// function set; nothing outside this query's directory changes.
#include "backends/duckdb/api/register.hpp"

#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
duckdb::TableFunctionSet {{create_fn}}();
namespace {
const BaseFunctionSetRegistrar kRegistrar{&{{create_fn}}};
} // namespace
} // namespace accelerator_codegen
