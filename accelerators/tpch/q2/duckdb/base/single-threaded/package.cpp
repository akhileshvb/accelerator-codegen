#include "backends/duckdb/api/query_runtime.hpp"
#include "tpch/q2/duckdb/base/query.hpp"

#include <memory>
#include <stdexcept>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen {
namespace {

Q2Parameters BindQ2Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q2 expects exactly 3 arguments: part_size, part_type_pattern, region_name");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q2 arguments must be non-NULL");
	}
	return MakeQ2Parameters(static_cast<int32_t>(inputs[0].GetValue<int64_t>()), inputs[1].GetValue<std::string>(),
	                      inputs[2].GetValue<std::string>());
}

void FillRegionNationsAndNames(duckdb::ClientContext &context, const std::string &region_name,
                               std::array<bool, 25> &region_nations,
                               std::unordered_map<int32_t, std::string> &nation_names) {
	region_nations.fill(false);
	nation_names.clear();
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT n.n_nationkey, n.n_name FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey WHERE r.r_name = '"
	    << duckdb::StringUtil::Replace(region_name, "'", "''") << "'";
	auto result = connection.Query(sql.str());
	if (result->HasError()) {
		throw duckdb::BinderException("failed to resolve region \"%s\": %s", region_name, result->GetError());
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		const auto nationkey = materialized.GetValue(0, row).GetValue<int32_t>();
		const auto nation_name = materialized.GetValue(1, row).GetValue<std::string>();
		if (nationkey >= 0 && nationkey < static_cast<int32_t>(region_nations.size())) {
			region_nations[static_cast<size_t>(nationkey)] = true;
		}
		nation_names.emplace(nationkey, nation_name);
	}
}

QueryBindResult BindQ2Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ2Parameters(input.inputs);
	FillRegionNationsAndNames(context, parameters.region_name, parameters.region_nations, parameters.nation_names);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q2Parameters>(parameters);
	result.input_scans.push_back(CreateQ2PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ2SupplierNumericScanSpec(parameters));
	result.input_scans.push_back(CreateQ2PartsuppScanSpec(parameters));
	result.input_scans.push_back(CreateQ2SupplierVarcharScanSpec(parameters));
	result.output_schema.names = {"s_acctbal", "s_name", "n_name", "p_partkey", "p_mfgr", "s_address", "s_phone",
	                              "s_comment"};
	result.output_schema.types = {duckdb::LogicalType::DECIMAL(15, 2), duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                              duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR,
	                              duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ2KernelInstance(const QueryParameters &parameters,
                                                          const QueryKernelFactoryData *) {
	auto &q2_parameters = dynamic_cast<const Q2Parameters &>(parameters);
	return CreateQ2Kernel(q2_parameters);
}

} // namespace

Q2Parameters MakeQ2Parameters(int32_t part_size, const std::string &part_type_pattern,
                              const std::string &region_name) {
	Q2Parameters parameters;
	parameters.part_size = part_size;
	parameters.part_type_pattern = part_type_pattern;
	parameters.region_name = region_name;
	return parameters;
}

NativeScanSpec CreateQ2PartScanSpec(const Q2Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_mfgr", "mfgr", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_type", "type", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_size", "size", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"p_size", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::INTEGER(parameters.part_size)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ2SupplierNumericScanSpec(const Q2Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"s_acctbal", "acctbal", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ2SupplierVarcharScanSpec(const Q2Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_address", "address", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_phone", "phone", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_comment", "comment", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ2PartsuppScanSpec(const Q2Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "partsupp";
	scan_spec.projected_columns = {
	    {"ps_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_supplycost", "supplycost", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
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

duckdb::unique_ptr<KernelInstance> CreateQ2FusedKernelInstance(const QueryParameters &parameters,
                                                               const QueryKernelFactoryData *) {
	if (!Q2FusedScansEnabled()) {
		return nullptr;
	}
	return CreateQ2Kernel(dynamic_cast<const Q2Parameters &>(parameters));
}

// Inputs 0..1 drained, then the fused partsupp and supplier scans.
void RunQ2FusedScan(duckdb::ClientContext &context, KernelInstance &kernel,
                    const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) {
	if (bound_inputs.size() != 4) {
		throw std::runtime_error("Q2 fused scans expect four bound inputs");
	}
	for (duckdb::idx_t input_id = 0; input_id < 2; input_id++) {
		DrainInto(context, kernel, *bound_inputs[input_id], input_id);
	}
	Q2RunFusedPartsuppScan(context, kernel, *bound_inputs[2]);
	Q2RunFusedSupplierVarcharScan(context, kernel, *bound_inputs[3]);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ2FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q2";
	package.overloads = {
	    {{duckdb::LogicalType::INTEGER, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ2Package;
	package.create_kernel = CreateQ2KernelInstance;
	package.create_fused_kernel = CreateQ2FusedKernelInstance;
	package.fused_scan = RunQ2FusedScan;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
