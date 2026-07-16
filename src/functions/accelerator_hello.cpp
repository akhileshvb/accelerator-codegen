#include "duckdb/function/table_function.hpp"

#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen {
namespace {

struct HelloGlobalState : public duckdb::GlobalTableFunctionState {
    bool emitted = false;
};

duckdb::unique_ptr<duckdb::FunctionData> AcceleratorHelloBind(
    duckdb::ClientContext &,
    duckdb::TableFunctionBindInput &,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<duckdb::string> &names) {
    names.emplace_back("value");
    return_types.emplace_back(duckdb::LogicalType::BIGINT);
    return nullptr;
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> AcceleratorHelloInit(
    duckdb::ClientContext &,
    duckdb::TableFunctionInitInput &) {
    return duckdb::make_uniq<HelloGlobalState>();
}

void AcceleratorHelloExec(duckdb::ClientContext &,
                          duckdb::TableFunctionInput &input,
                          duckdb::DataChunk &output) {
    auto &state = input.global_state->Cast<HelloGlobalState>();
    if (state.emitted) {
        return;
    }
    output.SetValue(0, 0, duckdb::Value::BIGINT(42));
    output.SetCardinality(1);
    state.emitted = true;
}

} // namespace

duckdb::TableFunction CreateAcceleratorHelloFunction() {
    return duckdb::TableFunction("accelerator_hello", {}, AcceleratorHelloExec, AcceleratorHelloBind,
                                 AcceleratorHelloInit);
}

} // namespace accelerator_codegen
