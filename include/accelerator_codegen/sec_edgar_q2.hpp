#pragma once

#include <cstdint>
#include <memory>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

enum class SecEdgarQ2InputId : duckdb::idx_t {
	SUB = 0,
	NUM = 1,
};

struct SecEdgarQ2SubChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct SecEdgarQ2NumChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t TAG_INDEX = 1;
	static constexpr duckdb::idx_t VALUE_INDEX = 2;
};

struct SecEdgarQ2Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ2Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ2Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ2ProfileCounters {
	duckdb::idx_t pre_rows = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t flat_chunks = 0;
	duckdb::idx_t dict_chunks = 0;
	duckdb::idx_t generic_chunks = 0;
	duckdb::idx_t distinct_inserts = 0;
	duckdb::idx_t active_groups = 0;
	duckdb::idx_t candidate_rows = 0;
	int64_t sub_scan_us = 0;
	int64_t num_max_us = 0;
	int64_t num_collect_us = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec CreateSecEdgarQ2SubScanSpec();
NativeScanSpec CreateSecEdgarQ2NumScanSpec();

class SecEdgarQ2Kernel : public KernelInstance {
public:
	explicit SecEdgarQ2Kernel(SecEdgarQ2Parameters params, SecEdgarQ2ProfileCounters *profile = nullptr);
	~SecEdgarQ2Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

protected:
	SecEdgarQ2Parameters params;
	SecEdgarQ2ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void ConsumeSubChunk(duckdb::DataChunk &input);
	void ConsumeNumChunk(duckdb::DataChunk &input);
	void RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void FinalizeCandidates();

	bool InputsConsumed() const;
	void MarkInputsConsumed();
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2Kernel(const SecEdgarQ2Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2ProfilingKernel(const SecEdgarQ2Parameters &params,
                                                                    SecEdgarQ2ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ2FunctionSet();

} // namespace accelerator_codegen
