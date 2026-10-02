// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <vector>

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

std::vector<TableFunctionSetFactory> &BaseFunctionSets() {
	static std::vector<TableFunctionSetFactory> factories;
	return factories;
}

} // namespace

void RegisterBaseFunctionSet(TableFunctionSetFactory factory) {
	BaseFunctionSets().push_back(factory);
}

void RegisterAcceleratorFunctions(duckdb::Connection &connection) {
    auto &context = *connection.context;
    auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
    const bool started_transaction = !connection.HasActiveTransaction();

    if (started_transaction) {
        connection.BeginTransaction();
    }

    try {
        for (const auto factory : BaseFunctionSets()) {
            RegisterTableFunction(catalog, context, factory());
        }
        accelerator_codegen::aux_state::engine::RegisterDuckdbAuxTableFunctions(
            connection);

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
