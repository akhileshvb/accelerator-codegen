#pragma once

#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/kernel.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"

namespace accelerator_codegen {

struct QueryParameters {
    virtual ~QueryParameters() = default;
    virtual std::unique_ptr<QueryParameters> Copy() const = 0;
    virtual bool Equals(const QueryParameters &other) const = 0;
};

struct QueryKernelFactoryData {
    virtual ~QueryKernelFactoryData() = default;
    virtual std::unique_ptr<QueryKernelFactoryData> Copy() const = 0;
    virtual bool Equals(const QueryKernelFactoryData &other) const = 0;
};

struct OutputSchema {
    duckdb::vector<duckdb::LogicalType> types;
    duckdb::vector<duckdb::string> names;
};

struct QueryBindResult {
    std::unique_ptr<QueryParameters> query_parameters;
    std::unique_ptr<QueryKernelFactoryData> kernel_factory_data;
    duckdb::vector<NativeScanSpec> input_scans;
    OutputSchema output_schema;
};

struct QueryFunctionOverload {
    duckdb::vector<duckdb::LogicalType> sql_argument_types;
};

using QueryBindCallback = QueryBindResult (*)(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input);
using CreateKernelCallback = duckdb::unique_ptr<KernelInstance> (*)(const QueryParameters &parameters,
                                                                    const QueryKernelFactoryData *factory_data);

struct QueryPackage {
    std::string function_name;
    duckdb::vector<QueryFunctionOverload> overloads;
    QueryBindCallback bind_query = nullptr;
    CreateKernelCallback create_kernel = nullptr;
};

duckdb::TableFunctionSet CreateRuntimeBackedFunctionSet(const QueryPackage &package);

} // namespace accelerator_codegen
