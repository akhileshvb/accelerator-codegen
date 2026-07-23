#include "accelerator_codegen/register.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace accelerator_codegen {

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
        duckdb::CreateTableFunctionInfo q6_info(CreateTpchQ6FunctionSet());
        catalog.CreateTableFunction(context, q6_info);

        duckdb::CreateTableFunctionInfo q1_info(CreateTpchQ1FunctionSet());
        catalog.CreateTableFunction(context, q1_info);

        duckdb::CreateTableFunctionInfo q3_info(CreateTpchQ3FunctionSet());
        catalog.CreateTableFunction(context, q3_info);

        duckdb::CreateTableFunctionInfo q4_info(CreateTpchQ4FunctionSet());
        catalog.CreateTableFunction(context, q4_info);

        duckdb::CreateTableFunctionInfo q9_info(CreateTpchQ9FunctionSet());
        catalog.CreateTableFunction(context, q9_info);

        duckdb::CreateTableFunctionInfo q12_info(CreateTpchQ12FunctionSet());
        catalog.CreateTableFunction(context, q12_info);

        duckdb::CreateTableFunctionInfo q14_info(CreateTpchQ14FunctionSet());
        catalog.CreateTableFunction(context, q14_info);

        duckdb::CreateTableFunctionInfo q17_info(CreateTpchQ17FunctionSet());
        catalog.CreateTableFunction(context, q17_info);

        duckdb::CreateTableFunctionInfo q19_info(CreateTpchQ19FunctionSet());
        catalog.CreateTableFunction(context, q19_info);

        duckdb::CreateTableFunctionInfo q5_info(CreateTpchQ5FunctionSet());
        catalog.CreateTableFunction(context, q5_info);

        duckdb::CreateTableFunctionInfo q7_info(CreateTpchQ7FunctionSet());
        catalog.CreateTableFunction(context, q7_info);

        duckdb::CreateTableFunctionInfo q10_info(CreateTpchQ10FunctionSet());
        catalog.CreateTableFunction(context, q10_info);

        duckdb::CreateTableFunctionInfo q11_info(CreateTpchQ11FunctionSet());
        catalog.CreateTableFunction(context, q11_info);

        duckdb::CreateTableFunctionInfo q13_info(CreateTpchQ13FunctionSet());
        catalog.CreateTableFunction(context, q13_info);

        duckdb::CreateTableFunctionInfo q16_info(CreateTpchQ16FunctionSet());
        catalog.CreateTableFunction(context, q16_info);

        duckdb::CreateTableFunctionInfo q18_info(CreateTpchQ18FunctionSet());
        catalog.CreateTableFunction(context, q18_info);

        duckdb::CreateTableFunctionInfo q22_info(CreateTpchQ22FunctionSet());
        catalog.CreateTableFunction(context, q22_info);

        duckdb::CreateTableFunctionInfo q15_info(CreateTpchQ15FunctionSet());
        catalog.CreateTableFunction(context, q15_info);

        duckdb::CreateTableFunctionInfo q21_info(CreateTpchQ21FunctionSet());
        catalog.CreateTableFunction(context, q21_info);

        duckdb::CreateTableFunctionInfo q20_info(CreateTpchQ20FunctionSet());
        catalog.CreateTableFunction(context, q20_info);

        duckdb::CreateTableFunctionInfo q8_info(CreateTpchQ8FunctionSet());
        catalog.CreateTableFunction(context, q8_info);

        duckdb::CreateTableFunctionInfo q2_info(CreateTpchQ2FunctionSet());
        catalog.CreateTableFunction(context, q2_info);

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
        duckdb::CreateTableFunctionInfo q6p_info(CreateTpchQ6ParallelFunctionSet());
        catalog.CreateTableFunction(context, q6p_info);

        duckdb::CreateTableFunctionInfo q1p_info(CreateTpchQ1ParallelFunctionSet());
        catalog.CreateTableFunction(context, q1p_info);

        duckdb::CreateTableFunctionInfo q3p_info(CreateTpchQ3ParallelFunctionSet());
        catalog.CreateTableFunction(context, q3p_info);

        duckdb::CreateTableFunctionInfo q4p_info(CreateTpchQ4ParallelFunctionSet());
        catalog.CreateTableFunction(context, q4p_info);

        duckdb::CreateTableFunctionInfo q9p_info(CreateTpchQ9ParallelFunctionSet());
        catalog.CreateTableFunction(context, q9p_info);

        duckdb::CreateTableFunctionInfo q12p_info(CreateTpchQ12ParallelFunctionSet());
        catalog.CreateTableFunction(context, q12p_info);

        duckdb::CreateTableFunctionInfo q14p_info(CreateTpchQ14ParallelFunctionSet());
        catalog.CreateTableFunction(context, q14p_info);

        duckdb::CreateTableFunctionInfo q15p_info(CreateTpchQ15ParallelFunctionSet());
        catalog.CreateTableFunction(context, q15p_info);

        duckdb::CreateTableFunctionInfo q17p_info(CreateTpchQ17ParallelFunctionSet());
        catalog.CreateTableFunction(context, q17p_info);

        duckdb::CreateTableFunctionInfo q19p_info(CreateTpchQ19ParallelFunctionSet());
        catalog.CreateTableFunction(context, q19p_info);

        duckdb::CreateTableFunctionInfo q5p_info(CreateTpchQ5ParallelFunctionSet());
        catalog.CreateTableFunction(context, q5p_info);

        duckdb::CreateTableFunctionInfo q7p_info(CreateTpchQ7ParallelFunctionSet());
        catalog.CreateTableFunction(context, q7p_info);

        duckdb::CreateTableFunctionInfo q22p_info(CreateTpchQ22ParallelFunctionSet());
        catalog.CreateTableFunction(context, q22p_info);

        duckdb::CreateTableFunctionInfo q11p_info(CreateTpchQ11ParallelFunctionSet());
        catalog.CreateTableFunction(context, q11p_info);

        duckdb::CreateTableFunctionInfo q13p_info(CreateTpchQ13ParallelFunctionSet());
        catalog.CreateTableFunction(context, q13p_info);

        duckdb::CreateTableFunctionInfo q16p_info(CreateTpchQ16ParallelFunctionSet());
        catalog.CreateTableFunction(context, q16p_info);

        duckdb::CreateTableFunctionInfo q2p_info(CreateTpchQ2ParallelFunctionSet());
        catalog.CreateTableFunction(context, q2p_info);

        duckdb::CreateTableFunctionInfo q8p_info(CreateTpchQ8ParallelFunctionSet());
        catalog.CreateTableFunction(context, q8p_info);

        duckdb::CreateTableFunctionInfo q10p_info(CreateTpchQ10ParallelFunctionSet());
        catalog.CreateTableFunction(context, q10p_info);

        duckdb::CreateTableFunctionInfo q18p_info(CreateTpchQ18ParallelFunctionSet());
        catalog.CreateTableFunction(context, q18p_info);

        duckdb::CreateTableFunctionInfo q20p_info(CreateTpchQ20ParallelFunctionSet());
        catalog.CreateTableFunction(context, q20p_info);

        duckdb::CreateTableFunctionInfo q21p_info(CreateTpchQ21ParallelFunctionSet());
        catalog.CreateTableFunction(context, q21p_info);
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
