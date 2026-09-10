#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q12/duckdb/base/query.hpp"

#include <memory>

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

duckdb::date_t ComputeExclusiveEndDate(duckdb::date_t start_date) {
	duckdb::interval_t one_year;
	one_year.months = 12;
	one_year.days = 0;
	one_year.micros = 0;
	return duckdb::Interval::Add(start_date, one_year);
}

Q12Parameters BindQ12Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 3) {
		throw duckdb::BinderException("accel_tpch_q12 expects exactly 3 arguments: shipmode1, shipmode2, start_date");
	}
	if (inputs[0].IsNull() || inputs[1].IsNull() || inputs[2].IsNull()) {
		throw duckdb::BinderException("accel_tpch_q12 arguments must be non-NULL");
	}
	return MakeQ12Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<std::string>(),
	                         inputs[2].GetValue<duckdb::date_t>());
}

QueryBindResult BindQ12Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ12Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q12Parameters>(parameters);
	result.input_scans.push_back(CreateQ12OrdersScanSpec(parameters));
	result.input_scans.push_back(CreateQ12LineitemScanSpec(parameters));
	result.output_schema.names = {"l_shipmode", "high_line_count", "low_line_count"};
	result.output_schema.types = {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::HUGEINT,
	                              duckdb::LogicalType::HUGEINT};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ12KernelInstance(const QueryParameters &parameters,
                                                           const QueryKernelFactoryData *) {
	auto &q12_parameters = dynamic_cast<const Q12Parameters &>(parameters);
	return CreateQ12Kernel(q12_parameters);
}

} // namespace

Q12Parameters MakeQ12Parameters(const std::string &shipmode1, const std::string &shipmode2,
                                duckdb::date_t start_date) {
	Q12Parameters parameters;
	parameters.shipmode1 = shipmode1;
	parameters.shipmode2 = shipmode2;
	parameters.start_date = start_date;
	parameters.end_date = ComputeExclusiveEndDate(start_date);
	return parameters;
}

NativeScanSpec CreateQ12OrdersScanSpec(const Q12Parameters &) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "orders";
	scan_spec.projected_columns = {
	    {"o_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"o_orderpriority", "orderpriority", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateQ12LineitemScanSpec(const Q12Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_orderkey", "orderkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_shipmode", "shipmode", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"l_commitdate", "commitdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_receiptdate", "receiptdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	    {"l_shipdate", "shipdate", duckdb::LogicalType::DATE, duckdb::PhysicalType::INT32},
	};
	// Push the half-open receipt-date window. Residual re-check remains in the kernel so
	// transaction-local FLAT vectors cannot silently bypass the predicate.
	scan_spec.filters = {
	    {"l_receiptdate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DATE(parameters.start_date)},
	    {"l_receiptdate", duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::DATE(parameters.end_date)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ12FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q12";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DATE}},
	};
	package.bind_query = BindQ12Package;
	package.create_kernel = CreateQ12KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
