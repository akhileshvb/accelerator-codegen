#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "accelerator_codegen/sec_edgar_q4_finalize.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

struct SecEdgarQ4NumChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t TAG_INDEX = 1;
	static constexpr duckdb::idx_t VERSION_INDEX = 2;
	static constexpr duckdb::idx_t VALUE_INDEX = 3;
	static constexpr duckdb::idx_t UOM_INDEX = 4;
};

struct SecEdgarQ4Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ4Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ4Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ4ProfileCounters {
	duckdb::idx_t pre_rows = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t flat_chunks = 0;
	duckdb::idx_t dict_chunks = 0;
	duckdb::idx_t generic_chunks = 0;
	duckdb::idx_t distinct_inserts = 0;
	duckdb::idx_t active_groups = 0;
	duckdb::idx_t candidate_rows = 0;
	int64_t tag_scan_us = 0;
	int64_t sub_scan_us = 0;
	int64_t pre_table_scan_us = 0;
	int64_t num_scan_us = 0;
	int64_t num_aggregate_us = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec CreateSecEdgarQ4TagScanSpec();
NativeScanSpec CreateSecEdgarQ4SubScanSpec();
NativeScanSpec CreateSecEdgarQ4PreScanSpec();
NativeScanSpec CreateSecEdgarQ4NumScanSpec();

using PostgresChunkScanner = std::function<void(
    const NativeScanSpec &scan_spec,
    const std::function<void(duckdb::DataChunk &chunk)> &consumer)>;

class SecEdgarQ4Kernel : public KernelInstance {
public:
	explicit SecEdgarQ4Kernel(SecEdgarQ4Parameters params, SecEdgarQ4ProfileCounters *profile = nullptr);
	~SecEdgarQ4Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;
	void RunPostgresChunkPrescan(const PostgresChunkScanner &scan);

protected:
	SecEdgarQ4Parameters params;
	SecEdgarQ4ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void RunTagScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void FinalizeGroups();

	bool InputsConsumed() const;
	void MarkInputsConsumed();

public:
	void ExportAuxPartitions(SecEdgarQ4SicCohortPartition &cohort, SecEdgarQ4PreEqBridgePartition &pre_bridge,
	                         SecEdgarQ4TagTlabelPartition &tag_tlabel, SecEdgarQ4GroupAggregatesPartition &aggregates,
	                         SecEdgarQ4OutputPartition &output);
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4Kernel(const SecEdgarQ4Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4ProfilingKernel(const SecEdgarQ4Parameters &params,
                                                                    SecEdgarQ4ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ4FunctionSet();

} // namespace accelerator_codegen
