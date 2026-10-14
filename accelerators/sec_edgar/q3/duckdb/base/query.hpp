#pragma once

#include <cstdint>
#include <memory>

#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "sec_edgar/q3/duckdb/aux/finalize.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

enum class SecEdgarQ3InputId : duckdb::idx_t {
	SUB = 0,
	NUM = 1,
};

struct SecEdgarQ3SubChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t CIK_INDEX = 2;
	static constexpr duckdb::idx_t FY_INDEX = 3;
};

struct SecEdgarQ3NumChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t VALUE_INDEX = 1;
	static constexpr duckdb::idx_t UOM_INDEX = 2;
};

struct SecEdgarQ3Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ3Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ3Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ3ProfileCounters {
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
	int64_t num_scan_us = 0;
	int64_t num_aggregate_us = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec CreateSecEdgarQ3SubScanSpec();
NativeScanSpec CreateSecEdgarQ3NumScanSpec();

class SecEdgarQ3Kernel : public KernelInstance {
public:
	explicit SecEdgarQ3Kernel(SecEdgarQ3Parameters params, SecEdgarQ3ProfileCounters *profile = nullptr);
	~SecEdgarQ3Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

	void ExportAuxPartitions(SecEdgarQ3Fy2022CohortPartition &cohort, SecEdgarQ3CikTotalsPartition &cik_totals,
	                         SecEdgarQ3OutputPartition &output);

protected:
	SecEdgarQ3Parameters params;
	SecEdgarQ3ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void ConsumeSubChunk(duckdb::DataChunk &input);
	void ConsumeNumChunk(duckdb::DataChunk &input);
	void RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void FinalizeGroups();

	bool InputsConsumed() const;
	void MarkInputsConsumed();
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3Kernel(const SecEdgarQ3Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3ProfilingKernel(const SecEdgarQ3Parameters &params,
                                                                    SecEdgarQ3ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ3FunctionSet();

} // namespace accelerator_codegen
