#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q20/duckdb/base/query.hpp"

#include <memory>
#include <sstream>
#include <utility>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/connection.hpp"

namespace accelerator_codegen {
namespace {

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_year;
	one_year.months = 12;
	one_year.days = 0;
	one_year.micros = 0;
	return duckdb::Interval::Add(start_date, one_year);
}

Q20Parameters BindQ20Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException(
		    "accel_tpch_q20 expects exactly 3 arguments: start_date, part_name_pattern, nation_name");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q20 arguments must be non-NULL");
	}
	return MakeQ20Parameters(inputs[0].GetValue<duckdb::date_t>(), inputs[1].GetValue<std::string>(),
	                         inputs[2].GetValue<std::string>());
}

int32_t ResolveNationKey(duckdb::ClientContext &context, const std::string &nation_name) {
	duckdb::Connection connection(*context.db);
	std::ostringstream sql;
	sql << "SELECT DISTINCT n_nationkey FROM nation WHERE n_name = '"
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

QueryBindResult BindQ20Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ20Parameters(input.inputs);
	parameters.target_nationkey = ResolveNationKey(context, parameters.nation_name);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q20Parameters>(parameters);
	result.input_scans.push_back(CreateQ20PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ20LineitemScanSpec(parameters));
	result.input_scans.push_back(CreateQ20PartsuppScanSpec(parameters));
	result.input_scans.push_back(CreateQ20SupplierScanSpec(parameters));
	result.output_schema.names = {"s_name", "s_address"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ20KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q20_parameters = dynamic_cast<const Q20Parameters &>(parameters);
	return CreateQ20Kernel(q20_parameters);
}

} // namespace

Q20Parameters MakeQ20Parameters(duckdb::date_t start_date, const std::string &part_name_pattern,
                                  const std::string &nation_name) {
	Q20Parameters parameters;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	parameters.part_name_pattern = part_name_pattern;
	parameters.nation_name = nation_name;
	return parameters;
}

NativeScanSpec CreateQ20LineitemScanSpec(const Q20Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	};
	scan_spec.filters = {
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"l_shipdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ20PartScanSpec(const Q20Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ20PartsuppScanSpec(const Q20Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "partsupp";
	scan_spec.projected_columns = {
	    {"ps_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"ps_availqty", "availqty", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	};
	return scan_spec;
}

NativeScanSpec CreateQ20SupplierScanSpec(const Q20Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "supplier";
	scan_spec.projected_columns = {
	    {"s_suppkey", "suppkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"s_name", "name", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_address", "address", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"s_nationkey", "nationkey", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	if (parameters.target_nationkey >= 0) {
		scan_spec.filters = {
		    {"s_nationkey", duckdb::ExpressionType::COMPARE_EQUAL,
		     duckdb::Value::INTEGER(parameters.target_nationkey)},
		};
	}
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ20FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q20";
	package.overloads = {
	    {{duckdb::LogicalType::DATE, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR}},
	};
	package.bind_query = BindQ20Package;
	package.create_kernel = CreateQ20KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
