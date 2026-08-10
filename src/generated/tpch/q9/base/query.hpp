#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

struct Q9Parameters final : public QueryParameters {
	std::string color;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q9Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q9Parameters &>(other);
		return color == rhs.color;
	}
};

struct Q9PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q9SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q9NationChunkLayout {
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q9OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 1;
};

struct Q9PartsuppChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t SUPPLYCOST_INDEX = 2;
};

struct Q9LineitemChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 2;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 3;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 4;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 5;
};

enum class Q9InputId : duckdb::idx_t {
	PART = 0,
	SUPPLIER = 1,
	NATION = 2,
	ORDERS = 3,
	PARTSUPP = 4,
	LINEITEM = 5,
};

struct Q9LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t flat_nullable = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q9ProfileCounters {
	duckdb::idx_t part_rows = 0;
	duckdb::idx_t part_selected = 0;
	duckdb::idx_t supplier_rows = 0;
	duckdb::idx_t nation_rows = 0;
	duckdb::idx_t orders_rows = 0;
	duckdb::idx_t partsupp_rows = 0;
	duckdb::idx_t partsupp_selected = 0;
	duckdb::idx_t lineitem_rows = 0;
	duckdb::idx_t lineitem_survivors = 0;
	duckdb::idx_t group_count = 0;
	duckdb::idx_t output_rows = 0;

	duckdb::idx_t selected_parts_bytes = 0;
	duckdb::idx_t supplier_map_bytes = 0;
	duckdb::idx_t nation_map_bytes = 0;
	duckdb::idx_t orders_map_bytes = 0;
	duckdb::idx_t partsupp_map_bytes = 0;
	duckdb::idx_t aggregate_bytes = 0;

	int64_t part_consume_ns = 0;
	int64_t supplier_consume_ns = 0;
	int64_t nation_consume_ns = 0;
	int64_t orders_consume_ns = 0;
	int64_t partsupp_consume_ns = 0;
	int64_t lineitem_consume_ns = 0;
	int64_t finalize_ns = 0;

	// Coarse subphase timers (chunk-level, not per-row).
	int64_t orders_init_ns = 0;
	int64_t orders_format_ns = 0;
	int64_t orders_insert_ns = 0;
	int64_t orders_overflow = 0;
	int64_t partsupp_format_ns = 0;
	int64_t partsupp_insert_ns = 0;
	int64_t lineitem_format_ns = 0;
	int64_t lineitem_probe_ns = 0;

	duckdb::idx_t line_part_hit = 0;
	duckdb::idx_t line_part_miss = 0;
	duckdb::idx_t line_supplier_hit = 0;
	duckdb::idx_t line_supplier_miss = 0;
	duckdb::idx_t line_order_hit = 0;
	duckdb::idx_t line_order_miss = 0;
	duckdb::idx_t line_partsupp_hit = 0;
	duckdb::idx_t line_partsupp_miss = 0;
	duckdb::idx_t partsupp_part_hit = 0;
	duckdb::idx_t partsupp_part_miss = 0;

	int64_t orderkey_min = 0;
	int64_t orderkey_max = 0;
	bool orderkey_domain_seen = false;

	Q9LayoutHistogram layout_o_orderkey;
	Q9LayoutHistogram layout_o_orderdate;
	Q9LayoutHistogram layout_l_partkey;
	Q9LayoutHistogram layout_l_suppkey;
	Q9LayoutHistogram layout_l_orderkey;
	Q9LayoutHistogram layout_l_quantity;
	Q9LayoutHistogram layout_l_extendedprice;
	Q9LayoutHistogram layout_l_discount;
	Q9LayoutHistogram layout_ps_partkey;
	Q9LayoutHistogram layout_p_name;

	bool used_dense_orders = false;
	bool used_dense_supplier = false;
	bool used_part_bitmap = false;
	bool used_packed_partsupp = false;
	bool used_flat_lineitem = false;
	bool used_dense_agg = false;

	duckdb::idx_t lineitem_flat_chunks = 0;
	duckdb::idx_t lineitem_generic_chunks = 0;
	duckdb::idx_t shared_sel_chunks = 0;
};

//! Production config is hardcoded (all retained toggles on). Override via compile-time
//! -DACCEL_EXPERIMENTAL_Q9_OPTS and ACCEL_Q9_* env vars:
//! - ACCEL_Q9_DENSE_ORDERS: dense orderkey->year array with overflow hash
//! - ACCEL_Q9_DENSE_SUPPLIER: dense suppkey->nationkey array
//! - ACCEL_Q9_PART_BITMAP: partkey membership bitmap
//! - ACCEL_Q9_PACKED_PARTSUPP: packed uint64 (partkey,suppkey) hash key
//! - ACCEL_Q9_FLAT_LINEITEM: FLAT/all-valid lineitem hot loop
//! - ACCEL_Q9_DENSE_AGG: dense (nation,year) aggregate slots
NativeScanSpec CreateQ9PartScanSpec(const Q9Parameters &parameters);
NativeScanSpec CreateQ9SupplierScanSpec(const Q9Parameters &parameters);
NativeScanSpec CreateQ9NationScanSpec(const Q9Parameters &parameters);
NativeScanSpec CreateQ9OrdersScanSpec(const Q9Parameters &parameters);
NativeScanSpec CreateQ9PartsuppScanSpec(const Q9Parameters &parameters);
NativeScanSpec CreateQ9LineitemScanSpec(const Q9Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ9Kernel(const Q9Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ9ProfilingKernel(const Q9Parameters &params, Q9ProfileCounters &counters);

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
duckdb::unique_ptr<KernelInstance> CreateQ9ParallelKernel(const Q9Parameters &params);
#endif

} // namespace accelerator_codegen
