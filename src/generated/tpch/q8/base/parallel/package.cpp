#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q8/base/query.hpp"

#include <memory>
#include <sstream>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen {
namespace {

Q8Parameters BindQ8ParallelParameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q8_parallel expects exactly 3 arguments: nation_name, region_name, part_type");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q8_parallel arguments must be non-NULL");
	}
	return MakeQ8Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(),
	                        inputs[2].GetValue<std::string>());
}

int32_t ResolveNationKey(duckdb::ClientContext &context, const std::string &nation_name) {
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT n_nationkey FROM nation WHERE n_name = '"
	    << duckdb::StringUtil::Replace(nation_name, "'", "''") << "'";
	auto result = connection.Query(sql.str());
	if (result->HasError()) {
		throw duckdb::BinderException("failed to resolve nation name \"%s\": %s", nation_name, result->GetError());
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	if (materialized.RowCount() != 1) {
		throw duckdb::BinderException("nation name \"%s\" did not resolve to exactly one nation key", nation_name);
	}
	return materialized.GetValue(0, 0).GetValue<int32_t>();
}

void ResolvePartKeys(duckdb::ClientContext &context, const std::string &part_type, std::vector<int64_t> &part_keys) {
	part_keys.clear();
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT p_partkey FROM part WHERE p_type LIKE '%"
	    << duckdb::StringUtil::Replace(part_type, "'", "''") << "'";
	auto result = connection.Query(sql.str());
	if (result->HasError()) {
		throw duckdb::BinderException("failed to resolve part keys for type suffix \"%s\": %s", part_type,
		                              result->GetError());
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	part_keys.reserve(materialized.RowCount());
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		part_keys.push_back(materialized.GetValue(0, row).GetValue<int64_t>());
	}
}

void FillRegionNations(duckdb::ClientContext &context, const std::string &region_name,
                       std::array<bool, 25> &region_nations) {
	region_nations.fill(false);
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT n_nationkey FROM nation n JOIN region r ON n.n_regionkey = r.r_regionkey WHERE r.r_name = '"
	    << duckdb::StringUtil::Replace(region_name, "'", "''") << "'";
	auto result = connection.Query(sql.str());
	if (result->HasError()) {
		throw duckdb::BinderException("failed to resolve region nations for \"%s\": %s", region_name,
		                              result->GetError());
	}
	auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t row = 0; row < materialized.RowCount(); row++) {
		const auto nationkey = materialized.GetValue(0, row).GetValue<int32_t>();
		if (nationkey >= 0 && nationkey < static_cast<int32_t>(region_nations.size())) {
			region_nations[static_cast<size_t>(nationkey)] = true;
		}
	}
}

QueryBindResult BindQ8ParallelPackage(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ8ParallelParameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);
	FillRegionNations(context, parameters.region_name, parameters.region_nations);
	ResolvePartKeys(context, parameters.part_type, parameters.bind_part_keys);

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

duckdb::unique_ptr<KernelInstance> CreateQ8ParallelKernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	auto &q8_parameters = dynamic_cast<const Q8Parameters &>(parameters);
	return CreateQ8ParallelKernel(q8_parameters);
}

} // namespace

duckdb::TableFunctionSet CreateTpchQ8ParallelFunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q8_parallel";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ8ParallelPackage;
	package.create_kernel = CreateQ8ParallelKernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
