#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/kernel.hpp"
#include "accelerator_codegen/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace accelerator_codegen {

struct Q16Parameters final : public QueryParameters {
	std::string brand;
	std::string type_pattern;
	std::array<int32_t, 8> sizes {};

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q16Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q16Parameters &>(other);
		return brand == rhs.brand && type_pattern == rhs.type_pattern && sizes == rhs.sizes;
	}
};

struct Q16SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t COMMENT_INDEX = 1;
};

struct Q16PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t BRAND_INDEX = 1;
	static constexpr duckdb::idx_t TYPE_INDEX = 2;
	static constexpr duckdb::idx_t SIZE_INDEX = 3;
};

struct Q16PartsuppChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
};

enum class Q16InputId : duckdb::idx_t {
	SUPPLIER = 0,
	PART = 1,
	PARTSUPP = 2,
};

struct Q16ProfileCounters {
	duckdb::idx_t supplier_rows_scanned = 0;
	duckdb::idx_t supplier_rows_excluded = 0;
	duckdb::idx_t part_rows_scanned = 0;
	duckdb::idx_t part_rows_matched = 0;
	duckdb::idx_t partsupp_rows_scanned = 0;
	duckdb::idx_t partsupp_rows_matched = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t supplier_flat_fast_rows = 0;
	duckdb::idx_t supplier_dict_fast_rows = 0;
	duckdb::idx_t part_flat_fast_rows = 0;
	duckdb::idx_t part_dict_fast_rows = 0;
	duckdb::idx_t partsupp_flat_fast_rows = 0;
	duckdb::idx_t partsupp_dict_fast_rows = 0;
	bool used_dense_state = false;
	bool used_two_part_like = false;
	bool used_prefix_like = false;
};

Q16Parameters MakeQ16Parameters(const std::string &brand, const std::string &type_pattern,
                                const std::array<int32_t, 8> &sizes);
NativeScanSpec CreateQ16SupplierScanSpec(const Q16Parameters &parameters);
NativeScanSpec CreateQ16PartScanSpec(const Q16Parameters &parameters);
NativeScanSpec CreateQ16PartsuppScanSpec(const Q16Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ16Kernel(const Q16Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ16ProfilingKernel(const Q16Parameters &params, Q16ProfileCounters *profile);
duckdb::unique_ptr<KernelInstance> CreateQ16ParallelKernel(const Q16Parameters &params);

} // namespace accelerator_codegen
