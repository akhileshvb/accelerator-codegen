#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q3/duckdb/base/query.hpp"

#include <memory>
#include <utility>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;
constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q3Parameters BindQ3Parameters(const duckdb::vector<duckdb::Value> &inputs) {
    if (inputs.size() != 2) {
        throw duckdb::BinderException("accel_tpch_q3 expects exactly 2 arguments");
    }

    Q3Parameters parameters;
    parameters.market_segment = inputs[0].GetValue<std::string>();
    parameters.cutoff_date = inputs[1].GetValue<duckdb::date_t>();
    return parameters;
}

QueryBindResult BindQ3Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
    auto parameters = BindQ3Parameters(input.inputs);

    QueryBindResult result;
    result.query_parameters = std::make_unique<Q3Parameters>(parameters);
    result.input_scans.push_back(CreateQ3CustomerScanSpec(parameters));
    result.input_scans.push_back(CreateQ3OrdersScanSpec(parameters));
    result.input_scans.push_back(CreateQ3LineitemScanSpec(parameters));
    result.output_schema.names = {"l_orderkey", "revenue", "o_orderdate", "o_shippriority"};
    result.output_schema.types = {
        duckdb::LogicalType::BIGINT,
        duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale),
        duckdb::LogicalType::DATE,
        duckdb::LogicalType::INTEGER,
    };
    return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ3KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
    auto &q3_parameters = dynamic_cast<const Q3Parameters &>(parameters);
    return CreateQ3Kernel(q3_parameters);
}

} // namespace

NativeScanSpec CreateQ3CustomerScanSpec(const Q3Parameters &parameters) {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "customer";
    scan_spec.projected_columns = {
        {"c_mktsegment", "mktsegment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
        {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
    };
    scan_spec.filters = {
        {"c_mktsegment", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value(parameters.market_segment)},
    };
    return scan_spec;
}

NativeScanSpec CreateQ3OrdersScanSpec(const Q3Parameters &parameters) {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "orders";
    scan_spec.projected_columns = {
        {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
        {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
        {"o_shippriority", "shippriority", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
    };
    scan_spec.filters = {
        {"o_orderdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.cutoff_date)},
    };
    return scan_spec;
}

NativeScanSpec CreateQ3LineitemScanSpec(const Q3Parameters &parameters) {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
        {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
         duckdb::PhysicalType::INT64},
        {"l_discount", "discount", duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
         duckdb::PhysicalType::INT64},
    };
    scan_spec.filters = {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHAN, duckdb::Value::DATE(parameters.cutoff_date)},
    };
    return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ3FunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q3";
    package.overloads = {
        {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
    };
    package.bind_query = BindQ3Package;
    package.create_kernel = CreateQ3KernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
