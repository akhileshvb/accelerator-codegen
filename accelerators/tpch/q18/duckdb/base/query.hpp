#pragma once

#include <cstdint>
#include <memory>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q18Parameters final : public QueryParameters {
	int64_t quantity_threshold = 0;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q18Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q18Parameters &>(other);
		return quantity_threshold == rhs.quantity_threshold;
	}
};

struct Q18LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 1;
};

struct Q18CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q18OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 2;
	static constexpr duckdb::idx_t TOTALPRICE_INDEX = 3;
};

enum class Q18InputId : duckdb::idx_t {
	LINEITEM_AGG = 0,
	ORDERS = 1,
	CUSTOMER = 2,
};

struct Q18ProfileCounters {
	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t orders_rows_matched = 0;
	duckdb::idx_t customer_rows_scanned = 0;
	duckdb::idx_t customer_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t lineitem_flat_fast_rows = 0;
	duckdb::idx_t lineitem_dict_fast_rows = 0;
	duckdb::idx_t orders_flat_fast_rows = 0;
	duckdb::idx_t orders_dict_fast_rows = 0;
	duckdb::idx_t customer_flat_fast_rows = 0;
	duckdb::idx_t customer_dict_fast_rows = 0;
	bool used_dense_state = false;
};

Q18Parameters MakeQ18Parameters(int64_t quantity_threshold);
NativeScanSpec CreateQ18LineitemAggScanSpec(const Q18Parameters &parameters);
NativeScanSpec CreateQ18OrdersScanSpec(const Q18Parameters &parameters);
NativeScanSpec CreateQ18CustomerScanSpec(const Q18Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ18Kernel(const Q18Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ18ProfilingKernel(const Q18Parameters &params, Q18ProfileCounters *profile);

duckdb::unique_ptr<KernelInstance> CreateQ18ParallelKernel(const Q18Parameters &params);

} // namespace accelerator_codegen
