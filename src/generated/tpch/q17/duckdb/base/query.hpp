#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q17Parameters final : public QueryParameters {
	std::string brand;
	std::string container;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q17Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q17Parameters &>(other);
		return brand == rhs.brand && container == rhs.container;
	}
};

struct Q17LineitemAggChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
};

struct Q17LineitemProbeChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
};

struct Q17PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t BRAND_INDEX = 1;
	static constexpr duckdb::idx_t CONTAINER_INDEX = 2;
};

enum class Q17InputId : duckdb::idx_t {
	LINEITEM_AGG = 0,
	PART = 1,
	LINEITEM_PROBE = 2,
};

struct Q17InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t generic_fallback_rows = 0;
};

struct Q17ProfileCounters {
	duckdb::idx_t lineitem_agg_rows_scanned = 0;
	duckdb::idx_t lineitem_probe_rows_scanned = 0;
	duckdb::idx_t part_rows_scanned = 0;
	duckdb::idx_t part_rows_matched = 0;
	duckdb::idx_t part_agg_cardinality = 0;
	duckdb::idx_t qualifying_part_cardinality = 0;
	duckdb::idx_t probe_rows_below_threshold = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t emitted_rows = 0;
	duckdb::idx_t part_overflow_entries = 0;
	duckdb::idx_t agg_overflow_entries = 0;
	duckdb::idx_t part_map_memory_bytes = 0;

	bool used_dense_agg = false;
	bool used_dense_qualifying = false;
	bool used_lineitem_flat_fast = false;
	bool used_lineitem_dict_fast = false;

	int64_t lineitem_agg_build_ns = 0;
	int64_t part_build_ns = 0;
	int64_t threshold_prepare_ns = 0;
	int64_t lineitem_probe_ns = 0;
	int64_t finalize_ns = 0;

	Q17InputProfile lineitem_agg;
	Q17InputProfile lineitem_probe;
	Q17InputProfile part;
};

Q17Parameters MakeQ17Parameters(const std::string &brand, const std::string &container);
NativeScanSpec CreateQ17LineitemAggScanSpec(const Q17Parameters &parameters);
NativeScanSpec CreateQ17PartScanSpec(const Q17Parameters &parameters);
NativeScanSpec CreateQ17LineitemProbeScanSpec(const Q17Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ17Kernel(const Q17Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ17ProfilingKernel(const Q17Parameters &params,
                                                            Q17ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ17ParallelKernel(const Q17Parameters &params);

} // namespace accelerator_codegen
