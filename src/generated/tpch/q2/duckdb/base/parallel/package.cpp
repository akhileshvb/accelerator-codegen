#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q2/duckdb/base/query.hpp"

#include <memory>
#include <sstream>
#include <unordered_map>

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen {
namespace {

Q2Parameters BindQ2ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q2_parallel expects exactly 3 arguments: part_size, part_type_pattern, region_name");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q2_parallel arguments must be non-NULL");
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

QueryBindResult BindQ2ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ2ParallelParameters(input.inputs);
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

duckdb::unique_ptr<KernelInstance> CreateQ2ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q2_parameters = dynamic_cast<const Q2Parameters &>(parameters);
	return CreateQ2ParallelKernel(q2_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ2ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q2_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::INTEGER, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ2ParallelPackage;
	package.create_kernel = CreateQ2ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
