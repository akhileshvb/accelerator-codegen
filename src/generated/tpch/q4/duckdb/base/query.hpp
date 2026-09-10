#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"

namespace duckdb {
class ClientContext;
class FunctionData;
} // namespace duckdb

namespace accelerator_codegen {

struct Q4Parameters final : public QueryParameters {
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q4Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q4Parameters &>(other);
		return start_date == rhs.start_date && end_date == rhs.end_date;
	}
};

struct Q4LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t COMMITDATE_INDEX = 1;
	static constexpr duckdb::idx_t RECEIPTDATE_INDEX = 2;
};

struct Q4OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 0;
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERPRIORITY_INDEX = 2;
};

enum class Q4InputId : duckdb::idx_t {
	LINEITEM = 0,
	ORDERS = 1,
};

struct Q4InputProfile {
	duckdb::idx_t total_chunks = 0;
	duckdb::idx_t flat_fast_path_chunks = 0;
	duckdb::idx_t dict_fast_path_chunks = 0;
	duckdb::idx_t generic_fallback_chunks = 0;
	duckdb::idx_t total_rows = 0;
	duckdb::idx_t flat_fast_path_rows = 0;
	duckdb::idx_t dict_fast_path_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
};

struct Q4LayoutHistogram {
	duckdb::idx_t flat_all_valid = 0;
	duckdb::idx_t dict_flat_all_valid = 0;
	duckdb::idx_t dict_flat_nullable = 0;
	duckdb::idx_t constant_all_valid = 0;
	duckdb::idx_t other = 0;
};

struct Q4ProfileCounters {
	Q4InputProfile lineitem;
	Q4InputProfile orders;
	Q4LayoutHistogram layout_l_orderkey;
	Q4LayoutHistogram layout_l_commitdate;
	Q4LayoutHistogram layout_l_receiptdate;
	Q4LayoutHistogram layout_o_orderdate;
	Q4LayoutHistogram layout_o_orderkey;
	Q4LayoutHistogram layout_o_orderpriority;

	duckdb::idx_t scanned_lineitems = 0;
	duckdb::idx_t qualifying_lineitems = 0;
	duckdb::idx_t duplicate_qualifying_lineitems = 0;
	duckdb::idx_t unique_qualifying_orderkeys = 0;
	duckdb::idx_t date_filter_survivors = 0;
	duckdb::idx_t residual_date_rejects = 0;
	duckdb::idx_t set_probes = 0;
	duckdb::idx_t set_hits = 0;
	duckdb::idx_t set_misses = 0;
	duckdb::idx_t priority_groups = 0;
	duckdb::idx_t fixed_priority_hits = 0;
	duckdb::idx_t generic_priority_hits = 0;
	duckdb::idx_t set_memory_bytes = 0;
	duckdb::idx_t set_capacity_bits = 0;
	duckdb::idx_t set_rehash_or_resize_count = 0;
	double load_factor = 0.0;

	bool used_fused_lineitem = false;
	duckdb::idx_t fused_vectors = 0;
	duckdb::idx_t fused_rows = 0;
	duckdb::idx_t fused_flat_identity_vectors = 0;
	duckdb::idx_t fused_nested_sel_vectors = 0;
	duckdb::idx_t fused_scalar_fallback_vectors = 0;
	int64_t fused_decode_ns = 0;
	int64_t fused_predicate_insert_ns = 0;

	duckdb::idx_t consecutive_key_skips = 0;
	duckdb::idx_t bitmap_writes = 0;
	duckdb::idx_t newly_set_bits = 0; // only filled when ACCEL_Q4_PROFILE_BITMAP=1

	int64_t predicate_ns = 0;
	int64_t set_insert_ns = 0;
	int64_t set_lookup_ns = 0;
	int64_t set_probe_ns = 0;
	int64_t residual_date_ns = 0;
	int64_t priority_agg_ns = 0;
	int64_t unified_format_ns = 0;
};

//! Production config is hardcoded (retained defaults). Override via compile-time
//! -DACCEL_EXPERIMENTAL_Q4_OPTS and ACCEL_Q4_* env vars.
//! Retained (on): BITMAP_SET, ORDERS_DICT_FAST, FIXED_PRIORITY, BITMAP_INLINE.
//! Not retained (default off):
//! - ACCEL_Q4_LINEITEM_FUSED: DuckDB-owned scan-consumer for lineitem.
//! - ACCEL_Q4_BYTE_STATE: byte-per-orderkey mark array (~6 MB) instead of bitmap
//! - ACCEL_Q4_RUN_AT_A_TIME: OR predicate across equal-orderkey runs; ≤1 state write/run
//! - ACCEL_Q4_PAIRED_DATE: with FUSED, Bitpacking FOR paired decode→bool (no DATE Vectors);
//!   alone, int32 days compare on the chunk path
bool Q4LineitemFusedScanEnabled();

Q4Parameters MakeQ4Parameters(duckdb::date_t start_date);
NativeScanSpec CreateQ4LineitemScanSpec(const Q4Parameters &parameters);
NativeScanSpec CreateQ4OrdersScanSpec(const Q4Parameters &parameters);

bool Q4LineitemChunkCanUseFlatFastPath(duckdb::DataChunk &input);
bool Q4OrdersChunkCanUseDictFastPath(duckdb::DataChunk &input);

//! Q4 kernel: optional fused lineitem scan-consumer, then orders DataChunk consume.
class Q4Kernel final : public KernelInstance {
public:
	explicit Q4Kernel(Q4Parameters params_p, Q4ProfileCounters *profile_p = nullptr);
	~Q4Kernel() override;

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override;
	void Finalize() override;
	bool ProduceOutput(duckdb::DataChunk &output) override;

	//! Drain lineitem via DataTable::ScanFilteredConsumer into the query-local bitmap.
	void RunFusedLineitemScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data);

	bool FusedLineitemScanned() const;

private:
	void ConsumeLineitemChunk(duckdb::DataChunk &input);
	void ConsumeLineitemFlatHot(duckdb::DataChunk &input);
	void ConsumeOrdersChunk(duckdb::DataChunk &input);

	struct State;
	Q4Parameters params;
	Q4ProfileCounters *profile;
	std::unique_ptr<State> state;
};

duckdb::unique_ptr<KernelInstance> CreateQ4Kernel(const Q4Parameters &params);
duckdb::unique_ptr<Q4Kernel> CreateQ4KernelTyped(const Q4Parameters &params, Q4ProfileCounters *profile = nullptr);
duckdb::unique_ptr<KernelInstance> CreateQ4ProfilingKernel(const Q4Parameters &params, Q4ProfileCounters &counters);

#ifdef ACCEL_ENABLE_PARALLEL_TPCH
duckdb::unique_ptr<KernelInstance> CreateQ4ParallelKernel(const Q4Parameters &params);
#endif

} // namespace accelerator_codegen
