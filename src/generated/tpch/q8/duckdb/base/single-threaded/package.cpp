#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q8/duckdb/base/query.hpp"
#include "tpch/q8/duckdb/base/q8_bind.hpp"

#include <memory>
#include <stdexcept>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen {
namespace {

Q8Parameters BindQ8Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q8 expects exactly 3 arguments: nation_name, region_name, part_type");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q8 arguments must be non-NULL");
	}
	return MakeQ8Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(),
	                        inputs[2].GetValue<std::string>());
}

QueryBindResult BindQ8Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ8Parameters(input.inputs);
	ResolveQ8BindParameters(context, parameters);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q8Parameters>(parameters);
	result.input_scans.push_back(CreateQ8PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ8SupplierScanSpec(parameters));
	result.input_scans.push_back(CreateQ8CustomerScanSpec(parameters));
	result.input_scans.push_back(CreateQ8OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ8LineitemScanSpec(parameters));
	result.output_schema.names = {"o_year", "mkt_share"};
	result.output_schema.types = {duckdb::LogicalType::BIGINT, duckdb::LogicalType::DOUBLE};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ8KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
	auto &q8_parameters = dynamic_cast<const Q8Parameters &>(parameters);
	return CreateQ8Kernel(q8_parameters);
}

} // namespace

Q8Parameters MakeQ8Parameters(const std::string &nation_name, const std::string &region_name,
                              const std::string &part_type) {
	Q8Parameters parameters;
	parameters.nation_name = nation_name;
	parameters.region_name = region_name;
	parameters.part_type = part_type;
	parameters.start_date = duckdb::Date::FromString("1995-01-01");
	parameters.end_date = duckdb::Date::FromString("1996-12-31");
	return parameters;
}

NativeScanSpec CreateQ8PartScanSpec(const Q8Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {};
	return scan_spec;
}

NativeScanSpec CreateQ8SupplierScanSpec(const Q8Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

NativeScanSpec CreateQ8LineitemScanSpec(const Q8Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ8OrdersScanSpec(const Q8Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderdate", "orderdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"o_orderdate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ8CustomerScanSpec(const Q8Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "customer";
	scan_spec.projected_columns = {
	    {"c_custkey", "custkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"c_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	return scan_spec;
}

namespace {

void DrainInto(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &input,
               duckdb::idx_t input_id) {
	auto source = CreateNativeChunkSource(context, input);
	while (source->Next()) {
		kernel.ConsumeInput(input_id, source->Chunk());
	}
}

duckdb::unique_ptr<KernelInstance> CreateQ8FusedKernelInstance(const QueryParameters &parameters,
                                                               const QueryKernelFactoryData *) {
	if (!Q8FusedScansEnabled()) {
		return nullptr;
	}
	return CreateQ8Kernel(dynamic_cast<const Q8Parameters &>(parameters));
}

// Inputs 0..1 drained, fused customer scan, orders drained, fused lineitem scan.
void RunQ8FusedScan(duckdb::ClientContext &context, KernelInstance &kernel,
                    const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) {
	if (bound_inputs.size() != 5) {
		throw std::runtime_error("Q8 fused scans expect five bound inputs");
	}
	for (duckdb::idx_t input_id = 0; input_id < 2; input_id++) {
		DrainInto(context, kernel, *bound_inputs[input_id], input_id);
	}
	Q8RunFusedCustomerScan(context, kernel, *bound_inputs[2]);
	DrainInto(context, kernel, *bound_inputs[3], static_cast<duckdb::idx_t>(Q8InputId::ORDERS));
	Q8RunFusedLineitemScan(context, kernel, *bound_inputs[4]);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ8FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q8";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ8Package;
	package.create_kernel = CreateQ8KernelInstance;
	package.create_fused_kernel = CreateQ8FusedKernelInstance;
	package.fused_scan = RunQ8FusedScan;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
