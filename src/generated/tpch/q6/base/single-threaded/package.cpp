#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q6/base/query.hpp"

#include <memory>
#include <string>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;
constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
    duckdb::interval_t one_year;
    one_year.months = duckdb::Interval::MONTHS_PER_YEAR;
    one_year.days = 0;
    one_year.micros = 0;
    return duckdb::Interval::Add(start_date, one_year);
}

int64_t ConvertToScaledDecimal(duckdb::ClientContext &context,
                               const duckdb::Value &input,
                               const std::string &parameter_name) {
    duckdb::Value cast_value;
    std::string error;
    const auto target_type = duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale);
    if (!input.TryCastAs(context, target_type, cast_value, &error, false)) {
        throw duckdb::BinderException("Could not cast %s to %s: %s", parameter_name, target_type.ToString(), error);
    }
    return cast_value.GetValueUnsafe<int64_t>();
}

Q6Parameters BindQ6Parameters(duckdb::ClientContext &context, const duckdb::vector<duckdb::Value> &inputs) {
    if (inputs.size() != 3) {
        throw duckdb::BinderException("accel_tpch_q6 expects exactly 3 arguments");
    }

    Q6Parameters parameters;
    parameters.start_date = inputs[0].GetValue<duckdb::date_t>();
    parameters.end_date = ComputeExclusiveEndDate(parameters.start_date);

    const auto discount_raw = ConvertToScaledDecimal(context, inputs[1], "discount");
    const auto quantity_raw = ConvertToScaledDecimal(context, inputs[2], "quantity");

    parameters.discount_lower_raw = discount_raw - 1;
    parameters.discount_upper_raw = discount_raw + 1;
    parameters.quantity_upper_raw = quantity_raw;
    return parameters;
}

duckdb::vector<NativeFilterSpec> CreateQ6Filters(const Q6Parameters &parameters) {
    return {
        {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::DATE(parameters.start_date)},
        {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
        {"l_discount", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_lower_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
        {"l_discount", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
         duckdb::Value::DECIMAL(parameters.discount_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
        {"l_quantity", duckdb::ExpressionType::COMPARE_LESSTHAN,
         duckdb::Value::DECIMAL(parameters.quantity_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale)},
    };
}

NativeScanSpec CreateQ6ScanSpec(const Q6Parameters &parameters) {
    NativeScanSpec scan_spec;
    scan_spec.table_name = "lineitem";
    scan_spec.projected_columns = {
        {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
        {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
        {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
    };
    scan_spec.filters = CreateQ6Filters(parameters);
    return scan_spec;
}

QueryBindResult BindQ6Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
    auto parameters = BindQ6Parameters(context, input.inputs);

    QueryBindResult result;
    result.query_parameters = std::make_unique<Q6Parameters>(parameters);
    result.input_scans.push_back(CreateQ6ScanSpec(parameters));
    result.output_schema.names.emplace_back("revenue");
    result.output_schema.types.emplace_back(duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale));
    return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ6KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
    auto &q6_parameters = dynamic_cast<const Q6Parameters &>(parameters);
    return CreateQ6Kernel(q6_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ6FunctionSet() {
    QueryPackage package;
    package.function_name = "accel_tpch_q6";
    package.overloads = {
        {{duckdb::LogicalType::DATE, duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
          duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale)}},
        {{duckdb::LogicalType::DATE, duckdb::LogicalType::DOUBLE, duckdb::LogicalType::DOUBLE}},
    };
    package.bind_query = BindQ6Package;
    package.create_kernel = CreateQ6KernelInstance;
    return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
