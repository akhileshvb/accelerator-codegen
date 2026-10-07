#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "tpch/q19/duckdb/base/query.hpp"

#include <algorithm>
#include <memory>

#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

Q19Parameters BindQ19Parameters(const duckdb::vector<duckdb::Value> &inputs) {
	if (inputs.size() != 6) {
		throw duckdb::BinderException(
		    "accel_tpch_q19 expects exactly 6 arguments: brand1, qty_low1, brand2, qty_low2, brand3, qty_low3");
	}
	for (duckdb::idx_t i = 0; i < inputs.size(); i++) {
		if (inputs[i].IsNull()) {
			throw duckdb::BinderException("accel_tpch_q19 arguments must be non-NULL");
		}
	}
	return MakeQ19Parameters(inputs[0].GetValue<std::string>(), inputs[1].GetValue<int64_t>(),
	                           inputs[2].GetValue<std::string>(), inputs[3].GetValue<int64_t>(),
	                           inputs[4].GetValue<std::string>(), inputs[5].GetValue<int64_t>());
}

QueryBindResult BindQ19Package(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input) {
	auto parameters = BindQ19Parameters(input.inputs);

	QueryBindResult result;
	result.query_parameters = std::make_unique<Q19Parameters>(parameters);
	result.input_scans.push_back(CreateQ19PartScanSpec(parameters));
	result.input_scans.push_back(CreateQ19LineitemScanSpec(parameters));
	result.output_schema.names = {"revenue"};
	result.output_schema.types = {duckdb::LogicalType::DECIMAL(38, 4)};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateQ19KernelInstance(const QueryParameters &parameters,
                                                         const QueryKernelFactoryData *) {
	auto &q19_parameters = dynamic_cast<const Q19Parameters &>(parameters);
	return CreateQ19Kernel(q19_parameters);
}

} // namespace

Q19Parameters MakeQ19Parameters(const std::string &brand1, int64_t qty_low1, const std::string &brand2,
                                int64_t qty_low2, const std::string &brand3, int64_t qty_low3) {
	Q19Parameters parameters;
	parameters.brand1 = brand1;
	parameters.qty_low1 = qty_low1;
	parameters.brand2 = brand2;
	parameters.qty_low2 = qty_low2;
	parameters.brand3 = brand3;
	parameters.qty_low3 = qty_low3;
	return parameters;
}

NativeScanSpec CreateQ19PartScanSpec(const Q19Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "part";
	scan_spec.projected_columns = {
	    {"p_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"p_brand", "brand", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_container", "container", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"p_size", "size", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	// Every branch requires 1 <= p_size <= 5/10/15, and p_brand is one of the three branch brands: push the
	// implied (superset) ranges; the kernel re-checks each branch exactly.
	const auto brand_lo = std::min({parameters.brand1, parameters.brand2, parameters.brand3});
	const auto brand_hi = std::max({parameters.brand1, parameters.brand2, parameters.brand3});
	scan_spec.filters = {
	    {"p_size", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(1)},
	    {"p_size", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(15)},
	    {"p_brand", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value(brand_lo)},
	    {"p_brand", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value(brand_hi)},
	};
	return scan_spec;
}

NativeScanSpec CreateQ19LineitemScanSpec(const Q19Parameters &parameters) {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "lineitem";
	scan_spec.projected_columns = {
	    {"l_partkey", "partkey", duckdb::LogicalType::BIGINT, duckdb::PhysicalType::INT64},
	    {"l_quantity", "quantity", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_extendedprice", "extendedprice", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_discount", "discount", duckdb::LogicalType::DECIMAL(15, 2), duckdb::PhysicalType::INT64},
	    {"l_shipmode", "shipmode", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"l_shipinstruct", "shipinstruct", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	// Conjuncts common to all three OR branches, pushed into the storage scan. The kernel keeps its
	// residual checks. 'AIR' <= l_shipmode <= 'AIR REG' admits exactly {AIR, AIR REG} among TPC-H modes
	// (and is only a superset otherwise; the residual check is exact).
	const int64_t qty_lo = std::min({parameters.qty_low1, parameters.qty_low2, parameters.qty_low3}) * 100;
	const int64_t qty_hi = (std::max({parameters.qty_low1, parameters.qty_low2, parameters.qty_low3}) + 10) * 100;
	scan_spec.filters = {
	    {"l_shipinstruct", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("DELIVER IN PERSON")},
	    {"l_shipmode", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value("AIR")},
	    {"l_shipmode", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value("AIR REG")},
	    {"l_quantity", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
	     duckdb::Value::DECIMAL(qty_lo, 15, 2)},
	    {"l_quantity", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::DECIMAL(qty_hi, 15, 2)},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateTpchQ19FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_tpch_q19";
	package.overloads = {
	    {{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR,
	      duckdb::LogicalType::BIGINT, duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT}},
	};
	package.bind_query = BindQ19Package;
	package.create_kernel = CreateQ19KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
