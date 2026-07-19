#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q14Parameters final : public QueryParameters {
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q14Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q14Parameters &>(other);
		return start_date == rhs.start_date && end_date == rhs.end_date;
	}
};

struct Q14PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t TYPE_INDEX = 1;
};

struct Q14LineitemChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 1;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 2;
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 3;
};

enum class Q14InputId : duckdb::idx_t {
	PART = 0,
	LINEITEM = 1,
};

struct Q14InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
};

struct Q14LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t flat_nullable = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_all_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q14ProfileCounters {
	Q14InputProfile part;
	Q14InputProfile lineitem;
	Q14LayoutHistogram layout_p_partkey;
	Q14LayoutHistogram layout_p_type;
	Q14LayoutHistogram layout_l_partkey;
	Q14LayoutHistogram layout_l_extendedprice;
	Q14LayoutHistogram layout_l_discount;
	Q14LayoutHistogram layout_l_shipdate;

	duckdb::idx_t part_rows_scanned = 0;
	duckdb::idx_t part_promo_count = 0;
	duckdb::idx_t part_non_promo_count = 0;
	duckdb::idx_t part_null_type_count = 0;
	duckdb::idx_t part_map_cardinality = 0;
	duckdb::idx_t part_map_memory_bytes = 0;
	duckdb::idx_t part_overflow_entries = 0;
	duckdb::idx_t part_dict_cache_hits = 0;
	bool used_dense_part = false;

	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t date_survivors = 0;
	duckdb::idx_t residual_date_checks = 0;
	duckdb::idx_t residual_date_skips = 0;
	duckdb::idx_t residual_date_rejects = 0;
	duckdb::idx_t join_hits = 0;
	duckdb::idx_t join_misses = 0;
	duckdb::idx_t promo_survivors = 0;
	duckdb::idx_t lineitem_shared_sel_chunks = 0;
	duckdb::idx_t emitted_rows = 0;
	bool result_is_null = false;
	bool denominator_is_zero = false;

	int64_t part_state_init_ns = 0;
	int64_t part_scan_decode_ns = 0;
	int64_t part_classify_ns = 0;
	int64_t part_insert_ns = 0;
	int64_t part_total_build_ns = 0;
	int64_t lineitem_scan_decode_ns = 0;
	int64_t lineitem_unified_ns = 0;
	int64_t lineitem_date_filter_ns = 0;
	int64_t lineitem_lookup_ns = 0;
	int64_t lineitem_arithmetic_ns = 0;
	int64_t lineitem_aggregate_ns = 0;
	int64_t finalize_ns = 0;
};

Q14Parameters MakeQ14Parameters(duckdb::date_t start_date);
NativeScanSpec CreateQ14PartScanSpec(const Q14Parameters &parameters);
NativeScanSpec CreateQ14LineitemScanSpec(const Q14Parameters &parameters);

//! Production config is hardcoded (dense/dict-fast on, skip-base-date-residual off).
//! Override via compile-time -DACCEL_EXPERIMENTAL_Q14_OPTS and ACCEL_Q14_* env vars.
bool Q14DensePartEnabled();
bool Q14PartDictFastEnabled();
bool Q14LineitemDictFastEnabled();
bool Q14SkipBaseDateResidualEnabled();

duckdb::unique_ptr<KernelInstance> CreateQ14Kernel(const Q14Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ14ProfilingKernel(const Q14Parameters &params, Q14ProfileCounters &counters);

} // namespace accelerator_codegen
