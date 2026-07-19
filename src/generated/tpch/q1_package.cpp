#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/tpch_q1.hpp"
#include "accelerator_codegen/tpch_q1_fused.hpp"

#include <cstdint>
#include <memory>

#include "duckdb/common/types/date.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;
constexpr uint8_t kSumQtyScale = 2;
constexpr uint8_t kSumBasePriceScale = 2;
constexpr uint8_t kSumDiscPriceScale = 4;
constexpr uint8_t kSumChargeScale = 6;
constexpr uint8_t kOutputDecimalWidth = 38;

duckdb::date_t Q1BaseDate() {
    return duckdb::Date::FromDate(1998, 12, 1);
}

Q1Parameters BindQ1Parameters(const duckdb::vector<duckdb::Value> &inputs) {
    if (inputs.size() != 1) {
        throw duckdb::BinderException("accel_tpch_q1 expects exactly 1 argument");
    }

    return MakeQ1Parameters(inputs[0].GetValue<int32_t>());
}
} // namespace

Q1Parameters MakeQ1Parameters(int32_t day_offset) {
    Q1Parameters parameters;
    parameters.day_offset = day_offset;
    parameters.cutoff_date = Q1BaseDate() - parameters.day_offset;
    return parameters;
}

NativeScanSpec CreateQ1ScanSpec(const Q1Parameters &parameters) {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        // Project shipdate first so the pushed filter column is leading in the native scan layout.
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_tax", "tax", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_returnflag", "returnflag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
        {"l_linestatus", "linestatus", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
    };
    scan_spec.filters = {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::DATE(parameters.cutoff_date)},
    };
    return scan_spec;
}

namespace {

QueryBindResult BindQ1Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
    auto parameters = BindQ1Parameters(input.inputs);

    QueryBindResult result;
    result.query_parameters = std::make_unique<Q1Parameters>(parameters);
    result.input_scans.push_back(CreateQ1ScanSpec(parameters));
    result.output_schema.names = {"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price", "sum_disc_price",
                                  "sum_charge", "avg_qty", "avg_price", "avg_disc", "count_order"};
    result.output_schema.types = {
        duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::VARCHAR,
        duckdb::LogicalType::DECIMAL(kOutputDecimalWidth, kSumQtyScale),
        duckdb::LogicalType::DECIMAL(kOutputDecimalWidth, kSumBasePriceScale),
        duckdb::LogicalType::DECIMAL(kOutputDecimalWidth, kSumDiscPriceScale),
        duckdb::LogicalType::DECIMAL(kOutputDecimalWidth, kSumChargeScale),
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::DOUBLE,
        duckdb::LogicalType::BIGINT,
    };
    return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ1KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
    auto &q1_parameters = dynamic_cast<const Q1Parameters &>(parameters);
    return CreateQ1FusedKernel(q1_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ1FunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q1";
    package.overloads = {
        {{duckdb::LogicalType::INTEGER}},
        {{duckdb::LogicalType::BIGINT}},
    };
    package.bind_query = BindQ1Package;
    package.create_kernel = CreateQ1KernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
