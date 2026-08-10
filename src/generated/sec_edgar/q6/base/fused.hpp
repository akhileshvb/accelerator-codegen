#pragma once

#include "sec_edgar/q6/base/query.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {
class FunctionData;
}

namespace accelerator_codegen {

class SecEdgarQ6FusedKernel final : public SecEdgarQ6Kernel {
public:
	explicit SecEdgarQ6FusedKernel(const SecEdgarQ6Parameters &params, SecEdgarQ6ProfileCounters *profile = nullptr);

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;

	void RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind);
};

void RunSecEdgarQ6FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind);

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6FusedKernel(const SecEdgarQ6Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6FusedProfilingKernel(const SecEdgarQ6Parameters &params,
                                                                          SecEdgarQ6ProfileCounters &counters);

} // namespace accelerator_codegen
