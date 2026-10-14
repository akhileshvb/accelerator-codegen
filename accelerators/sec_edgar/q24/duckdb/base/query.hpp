#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "sec_edgar/q24/duckdb/aux/finalize.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

struct SecEdgarQ24PreChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t TAG_INDEX = 1;
	static constexpr duckdb::idx_t VERSION_INDEX = 2;
};

struct SecEdgarQ24NumChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t TAG_INDEX = 1;
	static constexpr duckdb::idx_t VERSION_INDEX = 2;
	static constexpr duckdb::idx_t VALUE_INDEX = 3;
	static constexpr duckdb::idx_t UOM_INDEX = 4;
	static constexpr duckdb::idx_t DDATE_INDEX = 5;
};

struct SecEdgarQ24Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ24Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ24Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ24ProfileCounters {
	duckdb::idx_t pre_rows = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t flat_chunks = 0;
	duckdb::idx_t dict_chunks = 0;
	duckdb::idx_t generic_chunks = 0;
	duckdb::idx_t distinct_inserts = 0;
	duckdb::idx_t active_groups = 0;
	duckdb::idx_t candidate_rows = 0;
	int64_t pre_table_scan_us = 0;
	int64_t num_scan_us = 0;
	int64_t num_aggregate_us = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec CreateSecEdgarQ24PreScanSpec();
NativeScanSpec CreateSecEdgarQ24NumScanSpec();
NativeScanSpec CreateSecEdgarQ24NumFusedScanSpec();
NativeScanSpec CreateSecEdgarQ24AuxNumScanSpec();

using PostgresChunkScanner = std::function<void(
    const NativeScanSpec &scan_spec,
    const std::function<void(duckdb::DataChunk &chunk)> &consumer)>;

class SecEdgarQ24Kernel : public KernelInstance {
public:
	explicit SecEdgarQ24Kernel(SecEdgarQ24Parameters params, SecEdgarQ24ProfileCounters *profile = nullptr);
	~SecEdgarQ24Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;
	void RunPostgresChunkPrescan(const PostgresChunkScanner &scan);

	void ExportAuxPartitions(SecEdgarQ24PreMembershipPartition &pre_membership, SecEdgarQ24TagVersionPartition &tag_versions,
	                         SecEdgarQ24GroupAggregatesPartition &aggregates, SecEdgarQ24OutputPartition &output);

protected:
	SecEdgarQ24Parameters params;
	SecEdgarQ24ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunNumBufferPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void FinalizeGroups();

	bool InputsConsumed() const;
	void MarkInputsConsumed();
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24Kernel(const SecEdgarQ24Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24ProfilingKernel(const SecEdgarQ24Parameters &params,
                                                                    SecEdgarQ24ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ24FunctionSet();

} // namespace accelerator_codegen
