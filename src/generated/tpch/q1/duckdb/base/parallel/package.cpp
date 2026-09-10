#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q1/duckdb/base/query.hpp"
#include "tpch/q1/duckdb/base/fused.hpp"

#include <cstdint>
#include <memory>

#include "duckdb/common/types/date.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kSumQtyScale = 2;
constexpr uint8_t kSumBasePriceScale = 2;
constexpr uint8_t kSumDiscPriceScale = 4;
constexpr uint8_t kSumChargeScale = 6;
constexpr uint8_t kOutputDecimalWidth = 38;

Q1Parameters BindQ1Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 1) {
		throw duckdb::BinderException("accel_tpch_q1_parallel expects exactly 1 argument");
	}
	return MakeQ1Parameters(inputs[0].GetValue<int32_t>());
}

QueryBindResult BindQ1ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ1Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q1Parameters>(parameters);
	result.input_scans.push_back(CreateQ1ScanSpec(parameters));
	result.output_schema.names = {"l_returnflag", "l_linestatus", "sum_qty",      "sum_base_price", "sum_disc_price",
	                              "sum_charge",    "avg_qty",       "avg_price", "avg_disc",       "count_order"};
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

duckdb::unique_ptr<KernelInstance> CreateQ1ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q1_parameters = dynamic_cast<const Q1Parameters &>(parameters);
	return CreateQ1ParallelKernel(q1_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ1ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q1_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::INTEGER}},
	    {{duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ1ParallelPackage;
	package.create_kernel = CreateQ1ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
