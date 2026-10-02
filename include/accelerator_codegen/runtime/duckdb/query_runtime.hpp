// GENERATED BACKEND
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
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

// Optional query-owned scan path (fused scans).  When `create_fused_kernel`
// returns a kernel, the runtime calls `fused_scan` once with the bound inputs
// instead of draining each into ConsumeInput (the scan may still drain some
// inputs itself with CreateNativeChunkSource).  A null kernel means the
// default path: `create_kernel` and every bound input drained in order.
using CreateFusedKernelCallback = duckdb::unique_ptr<KernelInstance> (*)(const QueryParameters &parameters,
                                                                         const QueryKernelFactoryData *factory_data);
using FusedScanCallback = void (*)(duckdb::ClientContext &context, KernelInstance &kernel,
                                   const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs);

struct QueryPackage {
    std::string function_name;
    duckdb::vector<QueryFunctionOverload> overloads;
    QueryBindCallback bind_query = nullptr;
    CreateKernelCallback create_kernel = nullptr;
    CreateFusedKernelCallback create_fused_kernel = nullptr;
    FusedScanCallback fused_scan = nullptr;
};

duckdb::TableFunctionSet CreateRuntimeBackedFunctionSet(const QueryPackage &package);

} // namespace accelerator_codegen
