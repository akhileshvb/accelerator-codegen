// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include "accelerator_codegen/runtime/catalog/shared_query_engine.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace accelerator_codegen {

namespace {

void RegisterTableFunction(duckdb::Catalog &catalog, duckdb::ClientContext &context, duckdb::TableFunctionSet function_set) {
	duckdb::CreateTableFunctionInfo info(std::move(function_set));
	info.on_conflict = duckdb::OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateTableFunction(context, info);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ6FunctionSet();
duckdb::TableFunctionSet CreateTpchQ1FunctionSet();
duckdb::TableFunctionSet CreateTpchQ3FunctionSet();
duckdb::TableFunctionSet CreateTpchQ4FunctionSet();
duckdb::TableFunctionSet CreateTpchQ9FunctionSet();
duckdb::TableFunctionSet CreateTpchQ12FunctionSet();
duckdb::TableFunctionSet CreateTpchQ14FunctionSet();
duckdb::TableFunctionSet CreateTpchQ17FunctionSet();
duckdb::TableFunctionSet CreateTpchQ19FunctionSet();
duckdb::TableFunctionSet CreateTpchQ5FunctionSet();
duckdb::TableFunctionSet CreateTpchQ7FunctionSet();
duckdb::TableFunctionSet CreateTpchQ10FunctionSet();
duckdb::TableFunctionSet CreateTpchQ11FunctionSet();
duckdb::TableFunctionSet CreateTpchQ13FunctionSet();
duckdb::TableFunctionSet CreateTpchQ16FunctionSet();
duckdb::TableFunctionSet CreateTpchQ18FunctionSet();
duckdb::TableFunctionSet CreateTpchQ22FunctionSet();
duckdb::TableFunctionSet CreateTpchQ15FunctionSet();
duckdb::TableFunctionSet CreateTpchQ21FunctionSet();
duckdb::TableFunctionSet CreateTpchQ20FunctionSet();
duckdb::TableFunctionSet CreateTpchQ8FunctionSet();
duckdb::TableFunctionSet CreateTpchQ2FunctionSet();

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
duckdb::TableFunctionSet CreateTpchQ6ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ1ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ3ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ4ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ9ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ12ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ14ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ15ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ17ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ19ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ5ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ7ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ22ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ11ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ13ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ16ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ2ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ8ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ10ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ18ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ20ParallelFunctionSet();
duckdb::TableFunctionSet CreateTpchQ21ParallelFunctionSet();
#endif

void RegisterAcceleratorFunctions(duckdb::Connection &connection) {
    auto &context = *connection.context;
    auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
    const bool started_transaction = !connection.HasActiveTransaction();

    if (started_transaction) {
        connection.BeginTransaction();
    }

    try {
        RegisterTableFunction(catalog, context, CreateTpchQ6FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ1FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ3FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ4FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ9FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ12FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ14FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ17FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ19FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ5FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ7FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ10FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ11FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ13FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ16FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ18FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ22FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ15FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ21FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ20FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ8FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ2FunctionSet());

        
        accelerator_codegen::aux_state::engine::RegisterDuckdbAuxTableFunctions(
            connection);

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
        RegisterTableFunction(catalog, context, CreateTpchQ6ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ1ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ3ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ4ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ9ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ12ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ14ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ15ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ17ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ19ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ5ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ7ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ22ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ11ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ13ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ16ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ2ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ8ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ10ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ18ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ20ParallelFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ21ParallelFunctionSet());
#endif

        if (started_transaction) {
            connection.Commit();
        }
    } catch (...) {
        if (started_transaction && connection.HasActiveTransaction()) {
            connection.Rollback();
        }
        throw;
    }
}

} // namespace accelerator_codegen
