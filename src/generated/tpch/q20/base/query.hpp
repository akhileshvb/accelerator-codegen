#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen {

struct Q20Parameters final : public QueryParameters {
	duckdb::date_t start_date;
	duckdb::date_t end_date;
	std::string part_name_pattern;
	std::string nation_name;
	int32_t target_nationkey = -1;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q20Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q20Parameters &>(other);
		return start_date == rhs.start_date && end_date == rhs.end_date &&
		       part_name_pattern == rhs.part_name_pattern && nation_name == rhs.nation_name &&
		       target_nationkey == rhs.target_nationkey;
	}
};

struct Q20LineitemChunkLayout {
	static constexpr duckdb::idx_t SHIPDATE_INDEX = 0;
	static constexpr duckdb::idx_t PARTKEY_INDEX = 1;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 2;
	static constexpr duckdb::idx_t QUANTITY_INDEX = 3;
};

struct Q20PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q20PartsuppChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t AVAILQTY_INDEX = 2;
};

struct Q20SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t ADDRESS_INDEX = 2;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 3;
};

enum class Q20InputId : duckdb::idx_t {
	PART = 0,
	LINEITEM = 1,
	PARTSUPP = 2,
	SUPPLIER = 3,
};

Q20Parameters MakeQ20Parameters(duckdb::date_t start_date, const std::string &part_name_pattern,
                                const std::string &nation_name);
NativeScanSpec CreateQ20LineitemScanSpec(const Q20Parameters &parameters);
NativeScanSpec CreateQ20PartScanSpec(const Q20Parameters &parameters);
NativeScanSpec CreateQ20PartsuppScanSpec(const Q20Parameters &parameters);
NativeScanSpec CreateQ20SupplierScanSpec(const Q20Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ20Kernel(const Q20Parameters &params);

duckdb::unique_ptr<KernelInstance> CreateQ20ParallelKernel(const Q20Parameters &params);

} // namespace accelerator_codegen
