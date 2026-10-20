#pragma once

#include <cstdint>
#include <memory>

#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen {

struct SecEdgarQ1Parameters final : public QueryParameters {
	duckdb::idx_t row_estimate = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<SecEdgarQ1Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const SecEdgarQ1Parameters &>(other);
		return row_estimate == rhs.row_estimate;
	}
};

struct SecEdgarQ1PreChunkLayout {
	static constexpr duckdb::idx_t STMT_INDEX = 0;
	static constexpr duckdb::idx_t RFILE_INDEX = 1;
	static constexpr duckdb::idx_t ADSH_INDEX = 2;
	static constexpr duckdb::idx_t LINE_INDEX = 3;
};

struct SecEdgarQ1LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t flat_nullable = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t other = 0;
};

struct SecEdgarQ1ProfileCounters {
	duckdb::idx_t pre_chunks = 0;
	duckdb::idx_t pre_rows = 0;
	duckdb::idx_t qualifying_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t flat_chunks = 0;
	duckdb::idx_t dict_chunks = 0;
	duckdb::idx_t generic_chunks = 0;
	duckdb::idx_t distinct_inserts = 0;
	duckdb::idx_t active_groups = 0;
	duckdb::idx_t output_groups = 0;

	SecEdgarQ1LayoutHistogram layout_stmt;
	SecEdgarQ1LayoutHistogram layout_rfile;
	SecEdgarQ1LayoutHistogram layout_adsh;
	SecEdgarQ1LayoutHistogram layout_line;

	int64_t pre_scan_us = 0;
	int64_t pre_next_us = 0;
	int64_t pre_consume_us = 0;
	int64_t finalize_us = 0;
	int64_t distinct_insert_ns = 0;
};

NativeScanSpec CreateSecEdgarQ1ScanSpec();

class SecEdgarQ1Kernel : public KernelInstance {
public:
	explicit SecEdgarQ1Kernel(SecEdgarQ1Parameters params, SecEdgarQ1ProfileCounters *profile = nullptr);
	~SecEdgarQ1Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

protected:
	SecEdgarQ1Parameters params;
	SecEdgarQ1ProfileCounters *profile;

	struct State;
	std::unique_ptr<State> state;

	void ConsumePreChunk(duckdb::DataChunk &input);
	void ConsumePreVectors(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                       duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreDictDictFlatFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                                duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreDictDictFlatDict(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                                duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreDictDictDictFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                                duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreDictDictDictDict(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                                duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreFlatFlatFlatFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                                duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreFlatNullable(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                          duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreGeneric(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                       duckdb::Vector &line_vec, duckdb::idx_t count, duckdb::SelectionVector *filter_sel);
	void ConsumePreConsumableMixed(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
	                               duckdb::Vector &line_vec, duckdb::idx_t count,
	                               duckdb::SelectionVector *filter_sel);

	void FinalizeGroups();

	bool InputConsumed() const;
	void MarkInputConsumed();
};

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1Kernel(const SecEdgarQ1Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1ProfilingKernel(const SecEdgarQ1Parameters &params,
                                                                 SecEdgarQ1ProfileCounters &counters);

duckdb::TableFunctionSet CreateSecEdgarQ1FunctionSet();

} // namespace accelerator_codegen
