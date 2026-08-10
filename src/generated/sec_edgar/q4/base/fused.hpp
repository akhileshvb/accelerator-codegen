#pragma once

#include "sec_edgar/q4/base/query.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ4FusedKernel final : public SecEdgarQ4Kernel {
public:
	explicit SecEdgarQ4FusedKernel(const SecEdgarQ4Parameters &params, SecEdgarQ4ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind);
};

void RunSecEdgarQ4FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4FusedKernel(const SecEdgarQ4Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4FusedProfilingKernel(const SecEdgarQ4Parameters &params,
                                                                          SecEdgarQ4ProfileCounters &counters);

} // namespace accelerator_codegen
