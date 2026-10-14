#include "backends/duckdb/api/query_runtime.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"
#include "sec_edgar/q24/duckdb/base/fused.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/function_set.hpp"

namespace accelerator_codegen {
namespace {

QueryBindResult BindSecEdgarQ24Package(duckdb::ClientContext &context, duckdb::TableFunctionBindInput &) {
	SecEdgarQ24Parameters parameters;
	parameters.row_estimate = GetNativeScanTableRowCount(context, CreateSecEdgarQ24NumScanSpec());

	QueryBindResult result;
	result.query_parameters = std::make_unique<SecEdgarQ24Parameters>(parameters);
	result.input_scans.push_back(CreateSecEdgarQ24NumScanSpec());
	result.output_schema.names = {"tag", "version", "cnt", "total"};
	result.output_schema.types = {
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::VARCHAR,
	    duckdb::LogicalType::BIGINT,
	    duckdb::LogicalType::DOUBLE,
	};
	return result;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24KernelInstance(const QueryParameters &parameters,
                                                                   const QueryKernelFactoryData *) {
	return CreateSecEdgarQ24FusedKernel(dynamic_cast<const SecEdgarQ24Parameters &>(parameters));
}

} // namespace

NativeScanSpec CreateSecEdgarQ24PreScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "pre";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ24NumScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"ddate", "ddate", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	scan_spec.filters = {
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	    {"ddate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(20230101)},
	    {"ddate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(20231231)},
	};
	return scan_spec;
}

// Aux build scan: every num row with a non-NULL value; uom and ddate are
// not filtered because the state is keyed by (year, uom).
NativeScanSpec CreateSecEdgarQ24AuxNumScanSpec() {
	NativeScanSpec scan_spec = CreateSecEdgarQ24NumScanSpec();
	scan_spec.filters = {
	    {"value", duckdb::ExpressionType::OPERATOR_IS_NOT_NULL, duckdb::Value()},
	};
	return scan_spec;
}

NativeScanSpec CreateSecEdgarQ24NumFusedScanSpec() {
	NativeScanSpec scan_spec;
	scan_spec.table_name = "num";
	scan_spec.projected_columns = {
	    {"adsh", "adsh", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"tag", "tag", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"version", "version", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"value", "value", duckdb::LogicalType::DOUBLE, duckdb::PhysicalType::DOUBLE},
	    {"uom", "uom", duckdb::LogicalType::VARCHAR, duckdb::PhysicalType::VARCHAR},
	    {"ddate", "ddate", duckdb::LogicalType::INTEGER, duckdb::PhysicalType::INT32},
	};
	// Table filters: uom/ddate pushed to storage; value null-check in kernel.
	scan_spec.filters = {
	    {"uom", duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("USD")},
	    {"ddate", duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::INTEGER(20230101)},
	    {"ddate", duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::INTEGER(20231231)},
	};
	return scan_spec;
}

namespace {

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24FusedKernelInstance(const QueryParameters &parameters,
                                                                       const QueryKernelFactoryData *) {
	return CreateSecEdgarQ24FusedKernel(dynamic_cast<const SecEdgarQ24Parameters &>(parameters));
}

void RunSecEdgarQ24FusedScan(duckdb::ClientContext &context, KernelInstance &kernel,
                            const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) {
	if (bound_inputs.size() != 1) {
		throw std::runtime_error("SEC EDGAR fused scan expects exactly one bound input");
	}
	RunSecEdgarQ24FusedPreScan(kernel, context, *bound_inputs[0]);
}

} // namespace

duckdb::TableFunctionSet CreateSecEdgarQ24FunctionSet() {
	QueryPackage package;
	package.function_name = "accel_sec_edgar_q24";
	package.overloads = {{}};
	package.bind_query = BindSecEdgarQ24Package;
	package.create_kernel = CreateSecEdgarQ24KernelInstance;
	package.create_fused_kernel = CreateSecEdgarQ24FusedKernelInstance;
	package.fused_scan = RunSecEdgarQ24FusedScan;
	return CreateRuntimeBackedFunctionSet(package);
}

} // namespace accelerator_codegen
