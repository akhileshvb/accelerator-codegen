#pragma once

#include "sec_edgar/q3/base/query.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ3FusedKernel final : public SecEdgarQ3Kernel {
public:
	explicit SecEdgarQ3FusedKernel(const SecEdgarQ3Parameters &params, SecEdgarQ3ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind);
};

void RunSecEdgarQ3FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3FusedKernel(const SecEdgarQ3Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3FusedProfilingKernel(const SecEdgarQ3Parameters &params,
                                                                          SecEdgarQ3ProfileCounters &counters);

} // namespace accelerator_codegen
