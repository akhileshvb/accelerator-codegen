#include "accelerator_codegen/register.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace accelerator_codegen {

duckdb::TableFunction CreateAcceleratorHelloFunction();
duckdb::TableFunction CreateCountLineitemFunction();
duckdb::TableFunctionSet CreateTpchQ6FunctionSet();
duckdb::TableFunctionSet CreateTpchQ1FunctionSet();
duckdb::TableFunctionSet CreateTpchQ3FunctionSet();

void RegisterAcceleratorFunctions(duckdb::Connection &connection) {
    auto &context = *connection.context;
    auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
    const bool started_transaction = !connection.HasActiveTransaction();

    if (started_transaction) {
        connection.BeginTransaction();
    }

    try {
        duckdb::CreateTableFunctionInfo hello_info(CreateAcceleratorHelloFunction());
        catalog.CreateTableFunction(context, hello_info);

        duckdb::CreateTableFunctionInfo count_info(CreateCountLineitemFunction());
        catalog.CreateTableFunction(context, count_info);

        duckdb::CreateTableFunctionInfo q6_info(CreateTpchQ6FunctionSet());
        catalog.CreateTableFunction(context, q6_info);

        duckdb::CreateTableFunctionInfo q1_info(CreateTpchQ1FunctionSet());
        catalog.CreateTableFunction(context, q1_info);

        duckdb::CreateTableFunctionInfo q3_info(CreateTpchQ3FunctionSet());
        catalog.CreateTableFunction(context, q3_info);

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
