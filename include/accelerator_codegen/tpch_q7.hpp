#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q7Parameters final : public QueryParameters {
	std::string nation1;
	std::string nation2;
	duckdb::date_t shipdate_start;
	duckdb::date_t shipdate_end;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q7Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q7Parameters &>(other);
		return nation1 == rhs.nation1 && nation2 == rhs.nation2 && shipdate_start == rhs.shipdate_start &&
		       shipdate_end == rhs.shipdate_end;
	}
};

struct Q7NationChunkLayout {
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q7CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q7SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q7OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
};

struct Q7LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 2;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 3;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 4;
};

enum class Q7InputId : duckdb::idx_t {
	NATION = 0,
	CUSTOMER = 1,
	SUPPLIER = 2,
	ORDERS = 3,
	LINEITEM = 4,
};

struct Q7ProfileCounters {
	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t lineitem_flat_fast_rows = 0;
	duckdb::idx_t lineitem_dict_fast_rows = 0;
	bool used_dense_state = false;
	bool used_fixed_group_agg = false;
};

Q7Parameters MakeQ7Parameters(const std::string &nation1, const std::string &nation2);
NativeScanSpec CreateQ7NationScanSpec(const Q7Parameters &parameters);
NativeScanSpec CreateQ7CustomerScanSpec(const Q7Parameters &parameters);
NativeScanSpec CreateQ7SupplierScanSpec(const Q7Parameters &parameters);
NativeScanSpec CreateQ7OrdersScanSpec(const Q7Parameters &parameters);
NativeScanSpec CreateQ7LineitemScanSpec(const Q7Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ7Kernel(const Q7Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ7ProfilingKernel(const Q7Parameters &params, Q7ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ7ParallelKernel(const Q7Parameters &params);

} // namespace accelerator_codegen
