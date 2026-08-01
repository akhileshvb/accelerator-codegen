#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/sec_edgar_q4.hpp"
#include "accelerator_codegen/sec_edgar_q4_fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ4Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ4Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ4NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ4Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ4NumScanSpec());
	result.output_schema.names = {"sic", "tlabel", "stmt", "num_companies", "total_value", "avg_value"};
	result.output_schema.types = {
	    duckdb::LogicalType::INTEGER,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DOUBLE,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4KernelInstance(const QueryParameters &parameters,
                                                                  const QueryKernelFactoryData *) {
	return CreateSecEdgarQ4FusedKernel(dynamic_cast<const SecEdgarQ4Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ4TagScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "tag";
	scan_spec.projected_columns = {
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"abstract", "abstract", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"tlabel", "tlabel", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"abstract", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value::INTEGER(0)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ4SubScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "sub";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"cik", "cik", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	    {"sic", "sic", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"sic", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(4000)},
	    {"sic", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(4999)},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ4PreScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"stmt", "stmt", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"stmt", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("EQ")},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ4NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	scan_spec.filters = {
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	};
	return scan_spec;
}

duckdb::TableFunctionSet CreateSecEdgarQ4FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q4";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ4Package;
	package.create_kernel = CreateSecEdgarQ4KernelInstance;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
