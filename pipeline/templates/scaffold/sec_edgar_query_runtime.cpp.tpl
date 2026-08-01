#include "accelerator_codegen/sec_edgar_query_runtime.hpp"

namespace accelerator_codegen {

bool IsSecEdgarFusedQuery(const QueryPackage &package) {
	return package.function_name == "{{function_name}}";
}

bool TryInitSecEdgarFusedKernel(const QueryPackage &package, const QueryParameters &query_parameters,
                                QueryKernelFactoryData *kernel_factory_data,
                                std::unique_ptr<KernelInstance> &kernel_out) {
	(void)package;
	(void)query_parameters;
	(void)kernel_factory_data;
	(void)kernel_out;
	return false;
}

void ExecSecEdgarFusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                              const duckdb::FunctionData &bind_data) {
	(void)kernel;
	(void)context;
	(void)bind_data;
}

} // namespace accelerator_codegen
