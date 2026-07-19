#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace accelerator_codegen {

struct Q3Parameters final : public QueryParameters {
	std::string market_segment;
	duckdb::date_t cutoff_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q3Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q3Parameters &>(other);
		return market_segment == rhs.market_segment && cutoff_date == rhs.cutoff_date;
	}
};

struct Q3CustomerChunkLayout {
	static constexpr duckdb::idx_t MKTSEGMENT_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
};

struct Q3OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 0;
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 1;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 2;
	static constexpr duckdb::idx_t SHIPPRIORITY_INDEX = 3;
};

struct Q3LineitemChunkLayout {
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 0;
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 3;
};

enum class Q3InputId : duckdb::idx_t {
	CUSTOMER = 0,
	ORDERS = 1,
	LINEITEM = 2,
};

struct Q3LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q3InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
};

struct Q3ProfileCounters {
	Q3InputProfile customer;
	Q3InputProfile orders;
	Q3InputProfile lineitem;

	Q3LayoutHistogram layout_c_mktsegment;
	Q3LayoutHistogram layout_c_custkey;
	Q3LayoutHistogram layout_o_orderdate;
	Q3LayoutHistogram layout_o_orderkey;
	Q3LayoutHistogram layout_o_custkey;
	Q3LayoutHistogram layout_o_shippriority;
	Q3LayoutHistogram layout_l_shipdate;
	Q3LayoutHistogram layout_l_orderkey;
	Q3LayoutHistogram layout_l_extendedprice;
	Q3LayoutHistogram layout_l_discount;

	duckdb::idx_t qualifying_customers = 0;
	duckdb::idx_t qualifying_orders = 0;
	duckdb::idx_t qualifying_lineitems = 0;
	duckdb::idx_t aggregated_orders = 0;
	duckdb::idx_t touched_orders = 0;

	duckdb::idx_t customer_state_bytes = 0;
	duckdb::idx_t orders_state_bytes = 0;
	duckdb::idx_t customer_overflow = 0;
	duckdb::idx_t orders_overflow = 0;
	duckdb::idx_t slot_count = 0;

	int64_t customer_consume_ns = 0;
	int64_t orders_consume_ns = 0;
	int64_t lineitem_consume_ns = 0;
	int64_t finalize_ns = 0;
	int64_t slot_map_init_ns = 0;  // EnsureCapacity / memset / calloc
	int64_t orders_upsert_ns = 0;  // time inside UpsertOrder loops

	duckdb::idx_t slot_allocs = 0;       // new slots allocated
	duckdb::idx_t slot_map_pages_est = 0; // capacity * 4 / 4096

	bool used_dense_customers = false;
	bool used_dense_orders = false;
	bool used_topk_heap = false;
	bool used_dict_fast = false;
	bool used_trust_native = false;
	bool used_shared_sel = false;
	bool used_run_cache = false;
	bool used_slot_map = false;
	bool used_hot_cold = false;
	bool used_base_residual = false;
	bool used_cust_partial = false;
	bool used_slot_absent0 = false;
	bool used_slot_reserve = false;
	bool used_orders_shared_sel = false;

	duckdb::idx_t shared_sel_chunks = 0;
	duckdb::idx_t order_probes = 0;
	duckdb::idx_t order_probes_avoided = 0;
	duckdb::idx_t order_runs = 0;
	duckdb::idx_t order_run_rows = 0;
	duckdb::idx_t aggregate_writes = 0;
	duckdb::idx_t residual_date_checks = 0;
	duckdb::idx_t residual_date_skips = 0;
	duckdb::idx_t orders_shared_sel_chunks = 0;
};

//! Production config is hardcoded (retained defaults). Override via compile-time
//! -DACCEL_EXPERIMENTAL_Q3_OPTS and ACCEL_Q3_* env vars.
//! Retained (on): DENSE_CUST, DENSE_ORDERS, DICT_FAST, TOPK_HEAP, SHARED_SEL,
//!   SLOT_MAP, BASE_RESIDUAL, SLOT_RESERVE, ORDERS_SHARED_SEL
//! Not retained (default off):
//! - ACCEL_Q3_RUN_CACHE: run bookkeeping slower despite ~3.9 avg run / 2.4M probes avoided
//! - ACCEL_Q3_HOT_COLD: no-op flag; layout already hot/cold under SLOT_MAP
//! - ACCEL_Q3_CUST_PARTIAL: regresses several seeds
//! - ACCEL_Q3_TRUST_NATIVE: breaks transaction-local date updates
//! - ACCEL_Q3_SLOT_ABSENT0: calloc/0-sentinel — no win vs memset -1; regresses many seeds
//! Decode-integrated ScanFilteredConsumer / physical operator: not prototyped — profile shows
//! lineitem_next (native decode) ~24ms vs consume (probe/agg) ~13ms; fusion would still decode
//! orderkey/price/discount and Q4 already showed chunk-avoidance alone can regress.
NativeScanSpec CreateQ3CustomerScanSpec(const Q3Parameters &parameters);
NativeScanSpec CreateQ3OrdersScanSpec(const Q3Parameters &parameters);
NativeScanSpec CreateQ3LineitemScanSpec(const Q3Parameters &parameters);

bool Q3CustomerChunkCanUseFlatFastPath(duckdb::DataChunk &input);
bool Q3OrdersChunkCanUseFlatFastPath(duckdb::DataChunk &input);
bool Q3LineitemChunkCanUseFlatFastPath(duckdb::DataChunk &input);
bool Q3CustomerChunkCanUseDictFastPath(duckdb::DataChunk &input);
bool Q3OrdersChunkCanUseDictFastPath(duckdb::DataChunk &input);
bool Q3LineitemChunkCanUseDictFastPath(duckdb::DataChunk &input);
bool Q3CustomerChunkCanUsePartialDictFastPath(duckdb::DataChunk &input);

duckdb::unique_ptr<KernelInstance> CreateQ3Kernel(const Q3Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ3ProfilingKernel(const Q3Parameters &params, Q3ProfileCounters &counters);

} // namespace accelerator_codegen
