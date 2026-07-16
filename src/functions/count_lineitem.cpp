#include "accelerator_codegen/duckdb_scan_adapter.hpp"

#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/table_function.hpp"

namespace accelerator_codegen {
namespace {

struct CountLineitemGlobalState : public duckdb::GlobalTableFunctionState {
    bool emitted = false;
};

struct CountLineitemLocalState : public duckdb::LocalTableFunctionState {
    explicit CountLineitemLocalState(duckdb::unique_ptr<ChunkSource> source_p) : source(std::move(source_p)) {
    }

    duckdb::unique_ptr<ChunkSource> source;
};

duckdb::unique_ptr<duckdb::FunctionData> CountLineitemBind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<duckdb::string> &names) {
    names.emplace_back("row_count");
    return_types.emplace_back(duckdb::LogicalType::BIGINT);
    return CreateLineitemScanBindData(context, {"l_shipdate"});
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> CountLineitemInit(
    duckdb::ClientContext &,
    duckdb::TableFunctionInitInput &) {
    return duckdb::make_uniq<CountLineitemGlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> CountLineitemLocalInit(
    duckdb::ExecutionContext &context,
    duckdb::TableFunctionInitInput &input,
    duckdb::GlobalTableFunctionState *) {
    return duckdb::make_uniq<CountLineitemLocalState>(CreateChunkSource(context.client, *input.bind_data));
}

void CountLineitemDependency(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data) {
    AddNativeTableDependencies(dependencies, bind_data);
}

void CountLineitemExec(duckdb::ClientContext &context,
                       duckdb::TableFunctionInput &input,
                       duckdb::DataChunk &output) {
    auto &state = input.global_state->Cast<CountLineitemGlobalState>();
    if (state.emitted) {
        return;
    }

    auto &local_state = input.local_state->Cast<CountLineitemLocalState>();
    uint64_t row_count = 0;
    while (local_state.source->Next()) {
        if (context.interrupted) {
            throw duckdb::InterruptException();
        }
        row_count += local_state.source->Chunk().size();
    }

    output.SetValue(0, 0, duckdb::Value::BIGINT(duckdb::NumericCast<int64_t>(row_count)));
    output.SetCardinality(1);
    state.emitted = true;
}

} // namespace

duckdb::TableFunction CreateCountLineitemFunction() {
    duckdb::TableFunction function("accel_count_lineitem", {}, CountLineitemExec, CountLineitemBind,
                                   CountLineitemInit, CountLineitemLocalInit);
    function.dependency = CountLineitemDependency;
    return function;
}

} // namespace accelerator_codegen
