#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q11Parameters final : public QueryParameters {
	std::string nation_name;
	double fraction;
	int32_t target_nationkey = -1;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q11Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q11Parameters &>(other);
		return nation_name == rhs.nation_name && fraction == rhs.fraction &&
		       target_nationkey == rhs.target_nationkey;
	}
};

struct Q11NationChunkLayout {
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q11SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q11PartsuppChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t AVAILQTY_INDEX = 2;
	static constexpr duckdb::idx_t SUPPLYCOST_INDEX = 3;
};

enum class Q11InputId : duckdb::idx_t {
	SUPPLIER = 0,
	PARTSUPP = 1,
};

struct Q11ProfileCounters {
	duckdb::idx_t supplier_rows_scanned = 0;
	duckdb::idx_t supplier_rows_matched = 0;
	duckdb::idx_t partsupp_rows_scanned = 0;
	duckdb::idx_t partsupp_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t supplier_flat_fast_rows = 0;
	duckdb::idx_t supplier_dict_fast_rows = 0;
	duckdb::idx_t partsupp_flat_fast_rows = 0;
	duckdb::idx_t partsupp_dict_fast_rows = 0;
	bool used_dense_state = false;
};

Q11Parameters MakeQ11Parameters(const std::string &nation_name, double fraction);
NativeScanSpec CreateQ11NationScanSpec(const Q11Parameters &parameters);
NativeScanSpec CreateQ11SupplierScanSpec(const Q11Parameters &parameters);
NativeScanSpec CreateQ11PartsuppScanSpec(const Q11Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ11Kernel(const Q11Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ11ProfilingKernel(const Q11Parameters &params, Q11ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ11ParallelKernel(const Q11Parameters &params);

} // namespace accelerator_codegen
