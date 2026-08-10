#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "sec_edgar/q6/aux/finalize.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

struct SecEdgarQ6NumChunkLayout {
	static constexpr duckdb::idx_t ADSH_INDEX = 0;
	static constexpr duckdb::idx_t TAG_INDEX = 1;
	static constexpr duckdb::idx_t VERSION_INDEX = 2;
	static constexpr duckdb::idx_t VALUE_INDEX = 3;
	static constexpr duckdb::idx_t UOM_INDEX = 4;
};

struct SecEdgarQ6Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ6Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ6Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ6ProfileCounters {
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
	int64_t pre_table_scan_us = 0;
	int64_t num_scan_us = 0;
	int64_t num_aggregate_us = 0;
	int64_t pre_scan_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
};

NativeScanSpec CreateSecEdgarQ6SubScanSpec();
NativeScanSpec CreateSecEdgarQ6PreScanSpec();
NativeScanSpec CreateSecEdgarQ6NumScanSpec();

using PostgresChunkScanner = std::function<void(
    const NativeScanSpec &scan_spec,
    const std::function<void(duckdb::DataChunk &chunk)> &consumer)>;

class SecEdgarQ6Kernel : public KernelInstance {
public:
	explicit SecEdgarQ6Kernel(SecEdgarQ6Parameters params, SecEdgarQ6ProfileCounters *profile = nullptr);
	~SecEdgarQ6Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;
	void RunPostgresChunkPrescan(const PostgresChunkScanner &scan);

protected:
	SecEdgarQ6Parameters params;
	SecEdgarQ6ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);
	void FinalizeGroups();

	bool InputsConsumed() const;
	void MarkInputsConsumed();

public:
	void ExportAuxPartitions(SecEdgarQ6Fy2023CohortPartition &cohort, SecEdgarQ6PreIsBridgePartition &pre_bridge,
	                         SecEdgarQ6TagVersionPartition &tag_versions, SecEdgarQ6GroupAggregatesPartition &aggregates,
	                         SecEdgarQ6OutputPartition &output);
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6Kernel(const SecEdgarQ6Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6ProfilingKernel(const SecEdgarQ6Parameters &params,
                                                                    SecEdgarQ6ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ6FunctionSet();

} // namespace accelerator_codegen
