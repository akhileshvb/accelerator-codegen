#include "accelerator_codegen/register.hpp"

#include "accelerator_codegen/aux_state/accelerator_registry.hpp"
#include "accelerator_codegen/tpch_q14_aux.hpp"
#include "accelerator_codegen/tpch_q1_aux.hpp"
#include "accelerator_codegen/tpch_q3_aux.hpp"
#include "accelerator_codegen/tpch_q12_aux.hpp"
#include "accelerator_codegen/tpch_q10_aux.hpp"
#include "accelerator_codegen/tpch_q6_aux.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <mutex>
#include <unordered_set>

namespace accelerator_codegen {

namespace {

std::mutex g_catalog_register_mutex;
std::unordered_set<duckdb::DatabaseInstance *> g_catalog_registered_databases;

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
// BEGIN_SEC_EDGAR_REGISTRATIONS
duckdb::TableFunctionSet CreateSecEdgarQ1FunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ2FunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ3FunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ4FunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ6FunctionSet();
duckdb::TableFunctionSet CreateSecEdgarQ24FunctionSet();
// END_SEC_EDGAR_REGISTRATIONS
duckdb::TableFunctionSet CreateTpchQ14AuxFunctionSet();
duckdb::TableFunctionSet CreateQ14AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ14AuxRebuildPartitionFunctionSet();
duckdb::TableFunctionSet CreateTpchQ1AuxFunctionSet();
duckdb::TableFunctionSet CreateQ1AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ1AuxRebuildPartitionFunctionSet();
duckdb::TableFunctionSet CreateTpchQ3AuxFunctionSet();
duckdb::TableFunctionSet CreateQ3AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ3AuxRebuildPartitionFunctionSet();
duckdb::TableFunctionSet CreateTpchQ12AuxFunctionSet();
duckdb::TableFunctionSet CreateQ12AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ12AuxRebuildPartitionFunctionSet();
duckdb::TableFunctionSet CreateTpchQ10AuxFunctionSet();
duckdb::TableFunctionSet CreateQ10AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ10AuxRebuildPartitionFunctionSet();
duckdb::TableFunctionSet CreateTpchQ6AuxFunctionSet();
duckdb::TableFunctionSet CreateQ6AuxBuildFunctionSet();
duckdb::TableFunctionSet CreateQ6AuxRebuildPartitionFunctionSet();

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
    aux_state::InstallAcceleratorSubstrate(connection);

    auto *database = connection.context->db.get();
    {
        std::lock_guard<std::mutex> guard(g_catalog_register_mutex);
        if (!g_catalog_registered_databases.insert(database).second) {
            return;
        }
    }

    auto &context = *connection.context;
    auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
    const bool started_transaction = !connection.HasActiveTransaction();

    if (started_transaction) {
        connection.BeginTransaction();
    }

    try {
        RegisterQ14AuxAccelerator(connection);
        RegisterQ1AuxAccelerator(connection);
        RegisterQ3AuxAccelerator(connection);
        RegisterQ12AuxAccelerator(connection);
        RegisterQ10AuxAccelerator(connection);
        RegisterQ6AuxAccelerator(connection);

        RegisterTableFunction(catalog, context, CreateTpchQ6FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ1FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ3FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ4FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ9FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ12FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ14FunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ14AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ14AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ14AuxRebuildPartitionFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ1AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ1AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ1AuxRebuildPartitionFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ3AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ3AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ3AuxRebuildPartitionFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ12AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ12AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ12AuxRebuildPartitionFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ10AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ10AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ10AuxRebuildPartitionFunctionSet());

        RegisterTableFunction(catalog, context, CreateTpchQ6AuxFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ6AuxBuildFunctionSet());
        RegisterTableFunction(catalog, context, CreateQ6AuxRebuildPartitionFunctionSet());

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

        // BEGIN_SEC_EDGAR_REGISTRATIONS
        RegisterTableFunction(catalog, context, CreateSecEdgarQ1FunctionSet());
        RegisterTableFunction(catalog, context, CreateSecEdgarQ2FunctionSet());
        RegisterTableFunction(catalog, context, CreateSecEdgarQ3FunctionSet());
        RegisterTableFunction(catalog, context, CreateSecEdgarQ4FunctionSet());
        RegisterTableFunction(catalog, context, CreateSecEdgarQ6FunctionSet());
        RegisterTableFunction(catalog, context, CreateSecEdgarQ24FunctionSet());
        // END_SEC_EDGAR_REGISTRATIONS

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
        {
            std::lock_guard<std::mutex> guard(g_catalog_register_mutex);
            g_catalog_registered_databases.erase(database);
        }
        if (started_transaction && connection.HasActiveTransaction()) {
            connection.Rollback();
        }
        throw;
    }
}

} // namespace accelerator_codegen
