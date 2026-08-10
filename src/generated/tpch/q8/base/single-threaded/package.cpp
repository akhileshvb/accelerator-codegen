#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q8/base/query.hpp"

#include <memory>
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

QueryBindResult BindQ8Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ8Parameters(input.inputs);
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

duckdb::TableFunctionSet CreateTpchQ8FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q8";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ8Package;
	package.create_kernel = CreateQ8KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
