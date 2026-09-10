#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {
class ClientContext;
class FunctionData;
} // namespace duckdb

namespace accelerator_codegen {

struct Q2Parameters final : public QueryParameters {
	int32_t part_size = 0;
	std::string part_type_pattern;
	std::string region_name;
	std::array<bool, 25> region_nations {};
	std::unordered_map<int32_t, std::string> nation_names;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q2Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q2Parameters &>(other);
		return part_size == rhs.part_size && part_type_pattern == rhs.part_type_pattern &&
		       region_name == rhs.region_name && region_nations == rhs.region_nations &&
		       nation_names == rhs.nation_names;
	}
};

struct Q2PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t MFGR_INDEX = 1;
	static constexpr duckdb::idx_t TYPE_INDEX = 2;
	static constexpr duckdb::idx_t SIZE_INDEX = 3;
};

struct Q2SupplierNumericChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
	static constexpr duckdb::idx_t ACCTBAL_INDEX = 2;
};

struct Q2SupplierVarcharChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t ADDRESS_INDEX = 2;
	static constexpr duckdb::idx_t PHONE_INDEX = 3;
	static constexpr duckdb::idx_t COMMENT_INDEX = 4;
};

struct Q2SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t ADDRESS_INDEX = 2;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 3;
	static constexpr duckdb::idx_t ACCTBAL_INDEX = 4;
	static constexpr duckdb::idx_t PHONE_INDEX = 5;
	static constexpr duckdb::idx_t COMMENT_INDEX = 6;
};

struct Q2PartsuppChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 1;
	static constexpr duckdb::idx_t SUPPLYCOST_INDEX = 2;
};

enum class Q2InputId : duckdb::idx_t {
	PART = 0,
	SUPPLIER_NUMERIC = 1,
	PARTSUPP = 2,
	SUPPLIER_VARCHAR = 3,
};

Q2Parameters MakeQ2Parameters(int32_t part_size, const std::string &part_type_pattern,
                              const std::string &region_name);
NativeScanSpec CreateQ2PartScanSpec(const Q2Parameters &parameters);
NativeScanSpec CreateQ2SupplierNumericScanSpec(const Q2Parameters &parameters);
NativeScanSpec CreateQ2SupplierVarcharScanSpec(const Q2Parameters &parameters);
NativeScanSpec CreateQ2PartsuppScanSpec(const Q2Parameters &parameters);

bool Q2FusedScansEnabled();
void Q2RunFusedPartsuppScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data);
void Q2RunFusedSupplierVarcharScan(duckdb::ClientContext &context, KernelInstance &kernel,
                                   const duckdb::FunctionData &bind_data);

duckdb::unique_ptr<KernelInstance> CreateQ2Kernel(const Q2Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ2ParallelKernel(const Q2Parameters &params);

} // namespace accelerator_codegen
