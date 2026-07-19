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

struct Q12Parameters final : public QueryParameters {
	std::string shipmode1;
	std::string shipmode2;
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q12Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q12Parameters &>(other);
		return shipmode1 == rhs.shipmode1 && shipmode2 == rhs.shipmode2 && start_date == rhs.start_date &&
		       end_date == rhs.end_date;
	}
};

struct Q12OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t ORDERPRIORITY_INDEX = 1;
};

struct Q12LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t SHIPMODE_INDEX = 1;
	static constexpr duckdb::idx_t COMMITDATE_INDEX = 2;
	static constexpr duckdb::idx_t RECEIPTDATE_INDEX = 3;
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 4;
};

enum class Q12InputId : duckdb::idx_t {
	ORDERS = 0,
	LINEITEM = 1,
};

struct Q12InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
};

struct Q12LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t flat_nullable = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_all_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q12ProfileCounters {
	Q12InputProfile orders;
	Q12InputProfile lineitem;
	Q12LayoutHistogram layout_o_orderkey;
	Q12LayoutHistogram layout_o_orderpriority;
	Q12LayoutHistogram layout_l_orderkey;
	Q12LayoutHistogram layout_l_shipmode;
	Q12LayoutHistogram layout_l_commitdate;
	Q12LayoutHistogram layout_l_receiptdate;
	Q12LayoutHistogram layout_l_shipdate;

	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t orders_map_cardinality = 0;
	duckdb::idx_t orders_map_memory_bytes = 0;
	duckdb::idx_t orders_overflow_entries = 0;
	duckdb::idx_t orders_null_priority = 0;
	duckdb::idx_t orders_high_priority = 0;
	duckdb::idx_t orders_low_priority = 0;
	duckdb::idx_t orders_dict_cache_entries = 0;
	duckdb::idx_t orders_shared_sel_chunks = 0;

	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t shipmode_survivors = 0;
	duckdb::idx_t commit_lt_receipt_survivors = 0;
	duckdb::idx_t ship_lt_commit_survivors = 0;
	duckdb::idx_t receipt_window_survivors = 0;
	duckdb::idx_t residual_receipt_checks = 0;
	duckdb::idx_t residual_receipt_skips = 0;
	duckdb::idx_t residual_receipt_rejects = 0;
	duckdb::idx_t probe_hits = 0;
	duckdb::idx_t probe_misses = 0;
	duckdb::idx_t probe_neither_priority = 0;
	duckdb::idx_t fixed_slot_hits = 0;
	duckdb::idx_t generic_slot_hits = 0;
	duckdb::idx_t emitted_rows = 0;
	duckdb::idx_t lineitem_dict_cache_entries = 0;
	duckdb::idx_t lineitem_shared_sel_chunks = 0;
	bool used_dense_orders = false;

	// Coarse wall-clock phase timers (nanoseconds). No per-row clocks.
	int64_t orders_state_init_ns = 0;
	int64_t orders_scan_decode_ns = 0;
	int64_t orders_priority_decode_ns = 0;
	int64_t orders_priority_classify_ns = 0;
	int64_t orders_store_insert_ns = 0;
	int64_t orders_total_build_ns = 0;

	int64_t lineitem_scan_decode_ns = 0;
	int64_t lineitem_unified_ns = 0;
	int64_t lineitem_shipmode_ns = 0;
	int64_t lineitem_receipt_residual_ns = 0;
	int64_t lineitem_commit_lt_receipt_ns = 0;
	int64_t lineitem_ship_lt_commit_ns = 0;
	int64_t lineitem_probe_ns = 0;
	int64_t lineitem_aggregate_ns = 0;

	int64_t finalize_output_ns = 0;
	int64_t finalize_order_ns = 0;
};

Q12Parameters MakeQ12Parameters(const std::string &shipmode1, const std::string &shipmode2,
                                duckdb::date_t start_date);
NativeScanSpec CreateQ12OrdersScanSpec(const Q12Parameters &parameters);
NativeScanSpec CreateQ12LineitemScanSpec(const Q12Parameters &parameters);

//! Production config is hardcoded (dense/dict-fast on, skip-base-receipt-residual off).
//! Override via compile-time -DACCEL_EXPERIMENTAL_Q12_OPTS and ACCEL_Q12_* env vars.
bool Q12DenseOrdersEnabled();
bool Q12OrdersDictFastEnabled();
bool Q12LineitemDictFastEnabled();
bool Q12SkipBaseReceiptResidualEnabled();

duckdb::unique_ptr<KernelInstance> CreateQ12Kernel(const Q12Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ12ProfilingKernel(const Q12Parameters &params, Q12ProfileCounters &counters);

} // namespace accelerator_codegen
