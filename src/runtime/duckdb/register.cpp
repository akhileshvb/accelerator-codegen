// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/register.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
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

namespace {

// Opt-in DuckDB settings for benchmark processes, applied by every connection
// that registers the accelerator functions -- the base and the accelerated
// mode of a benchmark alike, so both run on the same engine configuration.
//
//   ACCEL_DUCKDB_CHECKPOINT_THRESHOLD=<size>   SET checkpoint_threshold
//
// With DuckDB's default (16 MiB) the commit of a multi-row UPDATE of a few
// thousand scattered num rows triggers an automatic checkpoint (1.4-1.8 s on
// SEC-EDGAR SF3); a larger threshold defers it to the database's close. The
// write itself (its rows, WAL record and durability) is unchanged.
void ApplyEngineSettingsFromEnvironment(duckdb::Connection &connection) {
    const char *threshold = std::getenv("ACCEL_DUCKDB_CHECKPOINT_THRESHOLD");
    if (!threshold || !*threshold) {
        return;
    }
    for (const char *c = threshold; *c; ++c) {
        if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '.' && *c != ' ') {
            throw std::invalid_argument("ACCEL_DUCKDB_CHECKPOINT_THRESHOLD: not a size: " +
                                        std::string(threshold));
        }
    }
    auto result = connection.Query(std::string("SET checkpoint_threshold = '") + threshold + "'");
    if (!result || result->HasError()) {
        throw std::runtime_error("SET checkpoint_threshold failed: " +
                                 (result ? result->GetError() : std::string("null result")));
    }
}

} // namespace

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
    ApplyEngineSettingsFromEnvironment(connection);
}

} // namespace accelerator_codegen
