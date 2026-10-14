#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q19Parameters final : public QueryParameters {
	std::string brand1;
	int64_t qty_low1 = 0;
	std::string brand2;
	int64_t qty_low2 = 0;
	std::string brand3;
	int64_t qty_low3 = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q19Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q19Parameters &>(other);
		return brand1 == rhs.brand1 && qty_low1 == rhs.qty_low1 && brand2 == rhs.brand2 &&
		       qty_low2 == rhs.qty_low2 && brand3 == rhs.brand3 && qty_low3 == rhs.qty_low3;
	}
};

struct Q19PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t BRAND_INDEX = 1;
	static constexpr duckdb::idx_t CONTAINER_INDEX = 2;
	static constexpr duckdb::idx_t SIZE_INDEX = 3;
};

struct Q19LineitemChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 3;
	static constexpr duckdb::idx_t SHIPMODE_INDEX = 4;
	static constexpr duckdb::idx_t SHIPINSTRUCT_INDEX = 5;
};

enum class Q19InputId : duckdb::idx_t {
	PART = 0,
	LINEITEM = 1,
};

struct Q19LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t flat_nullable = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_all_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q19InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t shared_sel_chunks = 0;
};

struct Q19ProfileCounters {
	duckdb::idx_t part_rows_scanned = 0;
	duckdb::idx_t part_rows_stored = 0;
	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_matched = 0;
	duckdb::idx_t part_map_cardinality = 0;
	duckdb::idx_t part_overflow_entries = 0;
	duckdb::idx_t part_map_memory_bytes = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t shipmode_rejected_rows = 0;
	duckdb::idx_t shipinstruct_rejected_rows = 0;
	duckdb::idx_t part_miss_rows = 0;

	bool used_dense_part = false;
	bool used_part_dict_fast = false;
	bool used_lineitem_dict_fast = false;

	int64_t part_state_init_ns = 0;
	int64_t part_build_ns = 0;
	int64_t lineitem_probe_ns = 0;
	int64_t finalize_ns = 0;

	Q19LayoutHistogram layout_p_partkey;
	Q19LayoutHistogram layout_p_brand;
	Q19LayoutHistogram layout_l_partkey;
	Q19LayoutHistogram layout_l_quantity;
	Q19InputProfile part;
	Q19InputProfile lineitem;
};

Q19Parameters MakeQ19Parameters(const std::string &brand1, int64_t qty_low1, const std::string &brand2,
                                int64_t qty_low2, const std::string &brand3, int64_t qty_low3);
NativeScanSpec CreateQ19PartScanSpec(const Q19Parameters &parameters);
NativeScanSpec CreateQ19LineitemScanSpec(const Q19Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ19Kernel(const Q19Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ19ProfilingKernel(const Q19Parameters &params,
                                                            Q19ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ19ParallelKernel(const Q19Parameters &params);

} // namespace accelerator_codegen
