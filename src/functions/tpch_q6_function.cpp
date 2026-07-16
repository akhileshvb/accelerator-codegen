#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/tpch_q6.hpp"

#include <string>
#include <utility>

#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kLineitemDecimalWidth = 15;
constexpr uint8_t kLineitemDecimalScale = 2;
constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

struct TpchQ6BindData : public duckdb::FunctionData {
    TpchQ6BindData(duckdb::unique_ptr<duckdb::FunctionData> scan_bind_data_p, Q6Parameters parameters_p)
        : scan_bind_data(std::move(scan_bind_data_p)), parameters(parameters_p) {
    }

    duckdb::unique_ptr<duckdb::FunctionData> scan_bind_data;
    Q6Parameters parameters;

    duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
        return duckdb::make_uniq<TpchQ6BindData>(scan_bind_data->Copy(), parameters);
    }

    bool Equals(const duckdb::FunctionData &other_p) const override {
        auto &other = other_p.Cast<TpchQ6BindData>();
        return parameters.start_date == other.parameters.start_date &&
               parameters.end_date == other.parameters.end_date &&
               parameters.discount_lower_raw == other.parameters.discount_lower_raw &&
               parameters.discount_upper_raw == other.parameters.discount_upper_raw &&
               parameters.quantity_upper_raw == other.parameters.quantity_upper_raw &&
               scan_bind_data->Equals(*other.scan_bind_data);
    }
};

struct TpchQ6GlobalState : public duckdb::GlobalTableFunctionState {
    bool emitted = false;
};

struct TpchQ6LocalState : public duckdb::LocalTableFunctionState {
    explicit TpchQ6LocalState(duckdb::unique_ptr<ChunkSource> source_p) : source(std::move(source_p)) {
    }

    duckdb::unique_ptr<ChunkSource> source;
};

const TpchQ6BindData &GetQ6BindData(const duckdb::FunctionData &bind_data) {
    return bind_data.Cast<TpchQ6BindData>();
}

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

duckdb::unique_ptr<duckdb::TableFilterSet> CreateQ6ScanFilters(duckdb::ClientContext &context,
                                                               const Q6Parameters &parameters) {
    auto filters = duckdb::make_uniq<duckdb::TableFilterSet>();

    auto push_filter = [&](duckdb::idx_t projected_column_index, duckdb::ExpressionType comparison_type,
                           duckdb::Value constant) {
        filters->PushFilter(duckdb::ColumnIndex(projected_column_index),
                            duckdb::make_uniq<duckdb::ConstantFilter>(comparison_type, std::move(constant)));
    };

    (void)context;
    push_filter(Q6LineitemChunkLayout::SHIPDATE_INDEX, duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                duckdb::Value::DATE(parameters.start_date));
    push_filter(Q6LineitemChunkLayout::SHIPDATE_INDEX, duckdb::ExpressionType::COMPARE_LESSTHAN,
                duckdb::Value::DATE(parameters.end_date));
    push_filter(Q6LineitemChunkLayout::DISCOUNT_INDEX, duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                duckdb::Value::DECIMAL(parameters.discount_lower_raw, kLineitemDecimalWidth, kLineitemDecimalScale));
    push_filter(Q6LineitemChunkLayout::DISCOUNT_INDEX, duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
                duckdb::Value::DECIMAL(parameters.discount_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale));
    push_filter(Q6LineitemChunkLayout::QUANTITY_INDEX, duckdb::ExpressionType::COMPARE_LESSTHAN,
                duckdb::Value::DECIMAL(parameters.quantity_upper_raw, kLineitemDecimalWidth, kLineitemDecimalScale));
    return filters;
}

duckdb::unique_ptr<duckdb::FunctionData> TpchQ6Bind(
    duckdb::ClientContext &context,
    duckdb::TableFunctionBindInput &input,
    duckdb::vector<duckdb::LogicalType> &return_types,
    duckdb::vector<duckdb::string> &names) {
    names.emplace_back("revenue");
    return_types.emplace_back(duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale));

    auto parameters = BindQ6Parameters(context, input.inputs);
    auto scan_bind_data = CreateLineitemScanBindData(
        context, {"l_shipdate", "l_discount", "l_quantity", "l_extendedprice"}, CreateQ6ScanFilters(context, parameters));
    return duckdb::make_uniq<TpchQ6BindData>(std::move(scan_bind_data), parameters);
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> TpchQ6Init(
    duckdb::ClientContext &,
    duckdb::TableFunctionInitInput &) {
    return duckdb::make_uniq<TpchQ6GlobalState>();
}

duckdb::unique_ptr<duckdb::LocalTableFunctionState> TpchQ6LocalInit(
    duckdb::ExecutionContext &context,
    duckdb::TableFunctionInitInput &input,
    duckdb::GlobalTableFunctionState *) {
    auto &bind_data = GetQ6BindData(*input.bind_data);
    return duckdb::make_uniq<TpchQ6LocalState>(CreateChunkSource(context.client, *bind_data.scan_bind_data));
}

void TpchQ6Dependency(duckdb::LogicalDependencyList &dependencies, const duckdb::FunctionData *bind_data) {
    auto &q6_bind_data = GetQ6BindData(*bind_data);
    AddNativeTableDependencies(dependencies, q6_bind_data.scan_bind_data.get());
}

void TpchQ6Exec(duckdb::ClientContext &,
                duckdb::TableFunctionInput &input,
                duckdb::DataChunk &output) {
    auto &global_state = input.global_state->Cast<TpchQ6GlobalState>();
    if (global_state.emitted) {
        return;
    }

    auto &bind_data = GetQ6BindData(*input.bind_data);
    auto &local_state = input.local_state->Cast<TpchQ6LocalState>();
    auto result = ExecuteQ6(*local_state.source, bind_data.parameters);

    if (result.is_null) {
        output.SetValue(0, 0, duckdb::Value(duckdb::LogicalType::DECIMAL(kRevenueDecimalWidth, kRevenueDecimalScale)));
    } else {
        output.SetValue(0, 0, duckdb::Value::DECIMAL(result.value, kRevenueDecimalWidth, kRevenueDecimalScale));
    }
    output.SetCardinality(1);
    global_state.emitted = true;
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ6FunctionSet() {
    duckdb::TableFunctionSet function_set("accel_tpch_q6");

    duckdb::TableFunction decimal_function(
        {duckdb::LogicalType::DATE, duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale),
         duckdb::LogicalType::DECIMAL(kLineitemDecimalWidth, kLineitemDecimalScale)},
        TpchQ6Exec, TpchQ6Bind, TpchQ6Init, TpchQ6LocalInit);
    decimal_function.dependency = TpchQ6Dependency;
    function_set.AddFunction(decimal_function);

    duckdb::TableFunction numeric_literal_function(
        {duckdb::LogicalType::DATE, duckdb::LogicalType::DOUBLE, duckdb::LogicalType::DOUBLE},
        TpchQ6Exec, TpchQ6Bind, TpchQ6Init, TpchQ6LocalInit);
    numeric_literal_function.dependency = TpchQ6Dependency;
    function_set.AddFunction(numeric_literal_function);

    return function_set;
}

} // namespace accelerator_codegen
