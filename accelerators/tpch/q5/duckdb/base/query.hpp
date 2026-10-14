#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q5Parameters final : public QueryParameters {
	std::string region_name;
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q5Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q5Parameters &>(other);
		return region_name == rhs.region_name && start_date == rhs.start_date && end_date == rhs.end_date;
	}
};

struct Q5RegionChunkLayout {
	static constexpr duckdb::idx_t REGIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q5NationChunkLayout {
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t REGIONKEY_INDEX = 2;
};

struct Q5CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q5SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q5OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 2;
};

struct Q5LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 2;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 3;
};

enum class Q5InputId : duckdb::idx_t {
	REGION = 0,
	NATION = 1,
	CUSTOMER = 2,
	SUPPLIER = 3,
	ORDERS = 4,
	LINEITEM = 5,
};

struct Q5ProfileCounters {
	duckdb::idx_t region_rows_scanned = 0;
	duckdb::idx_t nation_rows_scanned = 0;
	duckdb::idx_t customer_rows_scanned = 0;
	duckdb::idx_t supplier_rows_scanned = 0;
	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_matched = 0;
	duckdb::idx_t group_count = 0;
	duckdb::idx_t output_rows = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t lineitem_flat_fast_rows = 0;
	duckdb::idx_t lineitem_dict_fast_rows = 0;
	bool used_dense_state = false;
};

Q5Parameters MakeQ5Parameters(const std::string &region_name, duckdb::date_t start_date);
NativeScanSpec CreateQ5RegionScanSpec(const Q5Parameters &parameters);
NativeScanSpec CreateQ5NationScanSpec(const Q5Parameters &parameters);
NativeScanSpec CreateQ5CustomerScanSpec(const Q5Parameters &parameters);
NativeScanSpec CreateQ5SupplierScanSpec(const Q5Parameters &parameters);
NativeScanSpec CreateQ5OrdersScanSpec(const Q5Parameters &parameters);
NativeScanSpec CreateQ5LineitemScanSpec(const Q5Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ5Kernel(const Q5Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ5ProfilingKernel(const Q5Parameters &params, Q5ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ5ParallelKernel(const Q5Parameters &params);

} // namespace accelerator_codegen
