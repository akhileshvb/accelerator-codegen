#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/kernel.hpp"
#include "accelerator_codegen/runtime/duckdb/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace duckdb {
class ClientContext;
class FunctionData;
} // namespace duckdb

namespace accelerator_codegen {

struct Q8Parameters final : public QueryParameters {
	std::string nation_name;
	std::string region_name;
	std::string part_type;
	duckdb::date_t start_date;
	duckdb::date_t end_date;
	int32_t target_nationkey = -1;
	std::array<bool, 25> region_nations {};
	std::vector<int64_t> bind_part_keys;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q8Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q8Parameters &>(other);
		return nation_name == rhs.nation_name && region_name == rhs.region_name && part_type == rhs.part_type &&
		       start_date == rhs.start_date && end_date == rhs.end_date &&
		       target_nationkey == rhs.target_nationkey && region_nations == rhs.region_nations &&
		       bind_part_keys == rhs.bind_part_keys;
	}
};

struct Q8PartChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t TYPE_INDEX = 1;
};

struct Q8SupplierChunkLayout {
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

struct Q8LineitemChunkLayout {
	static constexpr duckdb::idx_t PARTKEY_INDEX = 0;
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 1;
	static constexpr duckdb::idx_t SUPPKEY_INDEX = 2;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 3;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 4;
};

struct Q8OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 2;
};

struct Q8CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 1;
};

enum class Q8InputId : duckdb::idx_t {
	PART = 0,
	SUPPLIER = 1,
	CUSTOMER = 2,
	ORDERS = 3,
	LINEITEM = 4,
};

bool Q8FusedScansEnabled();
bool Q8LineitemFusedScanEnabled();
void Q8RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data);
void Q8RunFusedOrdersScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data);
void Q8RunFusedLineitemScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data);

Q8Parameters MakeQ8Parameters(const std::string &nation_name, const std::string &region_name,
                              const std::string &part_type);
NativeScanSpec CreateQ8PartScanSpec(const Q8Parameters &parameters);
NativeScanSpec CreateQ8SupplierScanSpec(const Q8Parameters &parameters);
NativeScanSpec CreateQ8LineitemScanSpec(const Q8Parameters &parameters);
NativeScanSpec CreateQ8OrdersScanSpec(const Q8Parameters &parameters);
NativeScanSpec CreateQ8CustomerScanSpec(const Q8Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ8Kernel(const Q8Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ8ParallelKernel(const Q8Parameters &params);

} // namespace accelerator_codegen
