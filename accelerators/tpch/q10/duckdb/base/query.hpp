#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "backends/duckdb/api/kernel.hpp"
#include "backends/duckdb/api/query_runtime.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types/date.hpp"

namespace duckdb {
class ClientContext;
class FunctionData;
} // namespace duckdb

namespace accelerator_codegen {

struct Q10Parameters final : public QueryParameters {
	duckdb::date_t start_date;
	duckdb::date_t end_date;

	std::unique_ptr<QueryParameters> Copy() const override {
		return std::make_unique<Q10Parameters>(*this);
	}

	bool Equals(const QueryParameters &other) const override {
		auto &rhs = dynamic_cast<const Q10Parameters &>(other);
		return start_date == rhs.start_date && end_date == rhs.end_date;
	}
};

struct Q10NationChunkLayout {
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
};

struct Q10OrdersChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 1;
	static constexpr duckdb::idx_t ORDERDATE_INDEX = 2;
};

struct Q10LineitemChunkLayout {
	static constexpr duckdb::idx_t ORDERKEY_INDEX = 0;
	static constexpr duckdb::idx_t EXTENDEDPRICE_INDEX = 1;
	static constexpr duckdb::idx_t DISCOUNT_INDEX = 2;
	static constexpr duckdb::idx_t RETURNFLAG_INDEX = 3;
};

struct Q10CustomerChunkLayout {
	static constexpr duckdb::idx_t CUSTKEY_INDEX = 0;
	static constexpr duckdb::idx_t NAME_INDEX = 1;
	static constexpr duckdb::idx_t ACCTBAL_INDEX = 2;
	static constexpr duckdb::idx_t NATIONKEY_INDEX = 3;
	static constexpr duckdb::idx_t ADDRESS_INDEX = 4;
	static constexpr duckdb::idx_t PHONE_INDEX = 5;
	static constexpr duckdb::idx_t COMMENT_INDEX = 6;
};

enum class Q10InputId : duckdb::idx_t {
	NATION = 0,
	ORDERS = 1,
	LINEITEM = 2,
	CUSTOMER = 3,
};

struct Q10ProfileCounters {
	duckdb::idx_t orders_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_scanned = 0;
	duckdb::idx_t lineitem_rows_matched = 0;
	duckdb::idx_t customer_rows_stored = 0;
	duckdb::idx_t generic_fallback_rows = 0;
	duckdb::idx_t orders_flat_fast_rows = 0;
	duckdb::idx_t orders_dict_fast_rows = 0;
	duckdb::idx_t lineitem_flat_fast_rows = 0;
	duckdb::idx_t lineitem_dict_fast_rows = 0;
	duckdb::idx_t customer_fused_rows_scanned = 0;
	duckdb::idx_t customer_fused_rows_matched = 0;
	duckdb::idx_t lineitem_fused_rows_scanned = 0;
	duckdb::idx_t lineitem_fused_rows_matched = 0;
	duckdb::idx_t lineitem_fused_payload_rows = 0;
	bool used_fused_customer = false;
	bool used_fused_lineitem = false;
	bool used_dense_state = false;
};

//! Production Q10 uses ScanFilteredConsumer for the customer probe after revenue aggregation.
bool Q10FusedScansEnabled();
void Q10RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data);

Q10Parameters MakeQ10Parameters(duckdb::date_t start_date);
NativeScanSpec CreateQ10NationScanSpec(const Q10Parameters &parameters);
NativeScanSpec CreateQ10OrdersScanSpec(const Q10Parameters &parameters);
NativeScanSpec CreateQ10LineitemScanSpec(const Q10Parameters &parameters);
NativeScanSpec CreateQ10CustomerScanSpec(const Q10Parameters &parameters);

duckdb::unique_ptr<KernelInstance> CreateQ10Kernel(const Q10Parameters &params);
duckdb::unique_ptr<KernelInstance> CreateQ10ProfilingKernel(const Q10Parameters &params, Q10ProfileCounters *profile);

duckdb::unique_ptr<KernelInstance> CreateQ10ParallelKernel(const Q10Parameters &params);

} // namespace accelerator_codegen
