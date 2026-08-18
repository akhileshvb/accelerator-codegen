// GENERATED BACKEND
#pragma once

#include <memory>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {
class ClientContext;
class FunctionData;
} // namespace duckdb

namespace accelerator_codegen {

//! Returns true when the package should use the SEC EDGAR fused ScanFilteredConsumer path.
bool IsSecEdgarFusedQuery(const QueryPackage &package);

//! Create the production kernel for a fused SEC EDGAR query (called from query_runtime).
bool TryInitSecEdgarFusedKernel(const QueryPackage &package, const QueryParameters &query_parameters,
                                QueryKernelFactoryData *kernel_factory_data,
                                std::unique_ptr<KernelInstance> &kernel_out);

//! Run fused pre-table scan once before Finalize/ProduceOutput.
void ExecSecEdgarFusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                              const duckdb::FunctionData &bind_data);

} // namespace accelerator_codegen
