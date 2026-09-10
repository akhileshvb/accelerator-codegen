// GENERATED BACKEND
#include "sec_edgar/runtime.hpp"

#include "sec_edgar/q1/duckdb/base/query.hpp"
#include "sec_edgar/q1/duckdb/base/fused.hpp"
#include "sec_edgar/q2/duckdb/base/query.hpp"
#include "sec_edgar/q2/duckdb/base/fused.hpp"
#include "sec_edgar/q3/duckdb/base/query.hpp"
#include "sec_edgar/q3/duckdb/base/fused.hpp"
#include "sec_edgar/q4/duckdb/base/query.hpp"
#include "sec_edgar/q4/duckdb/base/fused.hpp"
#include "sec_edgar/q6/duckdb/base/query.hpp"
#include "sec_edgar/q6/duckdb/base/fused.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"
#include "sec_edgar/q24/duckdb/base/fused.hpp"

namespace accelerator_codegen {

bool IsSecEdgarFusedQuery(const QueryPackage &package) {
	return package.function_name == "accel_sec_edgar_q1" || package.function_name == "accel_sec_edgar_q2" ||
	       package.function_name == "accel_sec_edgar_q3" || package.function_name == "accel_sec_edgar_q4" ||
	       package.function_name == "accel_sec_edgar_q6" || package.function_name == "accel_sec_edgar_q24";
}

bool TryInitSecEdgarFusedKernel(const QueryPackage &package, const QueryParameters &query_parameters,
                                QueryKernelFactoryData *kernel_factory_data,
                                std::unique_ptr<KernelInstance> &kernel_out) {
	(void)kernel_factory_data;
	if (package.function_name == "accel_sec_edgar_q1") {
		auto &params = dynamic_cast<const SecEdgarQ1Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ1FusedKernel(params);
		return kernel_out != nullptr;
	}
	if (package.function_name == "accel_sec_edgar_q2") {
		auto &params = dynamic_cast<const SecEdgarQ2Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ2FusedKernel(params);
		return kernel_out != nullptr;
	}
	if (package.function_name == "accel_sec_edgar_q3") {
		auto &params = dynamic_cast<const SecEdgarQ3Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ3FusedKernel(params);
		return kernel_out != nullptr;
	}
	if (package.function_name == "accel_sec_edgar_q4") {
		auto &params = dynamic_cast<const SecEdgarQ4Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ4FusedKernel(params);
		return kernel_out != nullptr;
	}
	if (package.function_name == "accel_sec_edgar_q6") {
		auto &params = dynamic_cast<const SecEdgarQ6Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ6FusedKernel(params);
		return kernel_out != nullptr;
	}
	if (package.function_name == "accel_sec_edgar_q24") {
		auto &params = dynamic_cast<const SecEdgarQ24Parameters &>(query_parameters);
		kernel_out = CreateSecEdgarQ24FusedKernel(params);
		return kernel_out != nullptr;
	}
	return false;
}

void ExecSecEdgarFusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                              const duckdb::FunctionData &bind_data) {
	if (dynamic_cast<SecEdgarQ6FusedKernel *>(&kernel) != nullptr) {
		RunSecEdgarQ6FusedPreScan(kernel, context, bind_data);
		return;
	}
	if (dynamic_cast<SecEdgarQ24FusedKernel *>(&kernel) != nullptr) {
		RunSecEdgarQ24FusedPreScan(kernel, context, bind_data);
		return;
	}
	if (dynamic_cast<SecEdgarQ4FusedKernel *>(&kernel) != nullptr) {
		RunSecEdgarQ4FusedPreScan(kernel, context, bind_data);
		return;
	}
	if (dynamic_cast<SecEdgarQ3FusedKernel *>(&kernel) != nullptr) {
		RunSecEdgarQ3FusedPreScan(kernel, context, bind_data);
		return;
	}
	if (dynamic_cast<SecEdgarQ2FusedKernel *>(&kernel) != nullptr) {
		RunSecEdgarQ2FusedPreScan(kernel, context, bind_data);
		return;
	}
	RunSecEdgarQ1FusedPreScan(kernel, context, bind_data);
}

} // namespace accelerator_codegen
