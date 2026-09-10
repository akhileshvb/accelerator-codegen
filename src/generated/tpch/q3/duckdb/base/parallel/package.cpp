#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q3/duckdb/base/query.hpp"

#include <memory>
#include <utility>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;

Q3Parameters BindQ3Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 2) {
		throw duckdb::BinderException("accel_tpch_q3_parallel expects exactly 2 arguments");
	}

	Q3Parameters parameters;
	parameters.market_segment = inputs[0].GetValue<std::string>();
	parameters.cutoff_date = inputs[1].GetValue<duckdb::date_t>();
	return parameters;
}

QueryBindResult BindQ3ParallelPackage(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
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

duckdb::unique_ptr<KernelInstance> CreateQ3ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q3_parameters = dynamic_cast<const Q3Parameters &>(parameters);
	return CreateQ3ParallelKernel(q3_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ3ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q3_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ3ParallelPackage;
	package.create_kernel = CreateQ3ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
