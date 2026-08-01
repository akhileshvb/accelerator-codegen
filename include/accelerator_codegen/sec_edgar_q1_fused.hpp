#pragma once

#include "accelerator_codegen/sec_edgar_q1.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ1FusedKernel final : public SecEdgarQ1Kernel {
public:
	explicit SecEdgarQ1FusedKernel(const SecEdgarQ1Parameters &params, SecEdgarQ1ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
};

void RunSecEdgarQ1FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &bind_data);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1FusedKernel(const SecEdgarQ1Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1FusedProfilingKernel(const SecEdgarQ1Parameters &params,
                                                                        SecEdgarQ1ProfileCounters &counters);

} // namespace accelerator_codegen
