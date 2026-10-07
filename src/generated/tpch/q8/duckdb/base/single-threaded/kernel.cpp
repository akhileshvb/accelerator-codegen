#include "tpch/q8/duckdb/base/query.hpp"
#include "tpch/q8/duckdb/base/q8_common.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

struct Q8OutputRow {
	int64_t year = 0;
	double mkt_share = 0.0;
};

class Q8Kernel final : public KernelInstance {
public:
	explicit Q8Kernel(Q8Parameters params_p) : params(std::move(params_p)) {
		has_region_nations = q8::HasRegionNations(params);
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q8InputId>(input_id)) {
		case Q8InputId::PART:
			scratch_keys.clear();
			q8::CollectPartKeys(input, params.part_type, scratch_keys);
			for (const auto key : scratch_keys) {
				tables.parts.Set(key);
			}
			return;
		case Q8InputId::SUPPLIER:
			q8::ForEachSupplier(input, [&](int64_t suppkey, int32_t nationkey) { tables.suppliers.Store(suppkey, nationkey); });
			return;
		case Q8InputId::CUSTOMER:
			if (!has_region_nations) {
				return;
			}
			scratch_keys.clear();
			q8::CollectCustomerKeys(input, params.region_nations, scratch_keys);
			for (const auto key : scratch_keys) {
				tables.customers.Set(key);
			}
			return;
		case Q8InputId::ORDERS:
			if (!has_region_nations) {
				return;
			}
			scratch_orders.clear();
			q8::CollectOrders(input, tables.customers, scratch_orders);
			for (const auto &entry : scratch_orders) {
				tables.orders.Set(entry.orderkey, entry.slot);
			}
			return;
		case Q8InputId::LINEITEM:
			if (!has_region_nations) {
				return;
			}
			q8::ProcessLineitemChunk(tables, params.target_nationkey, input, volume_by_year);
			return;
		default:
			throw duckdb::InternalException("Q8 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		for (int slot = 0; slot < q8::kYearSlots; slot++) {
			const auto &state = volume_by_year[static_cast<size_t>(slot)];
			if (state.total_volume == 0) {
				continue;
			}
			Q8OutputRow row;
			row.year = q8::kMinYear + slot;
			row.mkt_share = q8::ComputeMarketShare(state);
			output_rows.push_back(row);
		}
		std::sort(output_rows.begin(), output_rows.end(),
		          [](const Q8OutputRow &lhs, const Q8OutputRow &rhs) { return lhs.year < rhs.year; });
		output_offset = 0;
		finalized = true;
	}

	bool ProduceOutput(duckdb::DataChunk &output) override {
		if (!finalized || output_offset >= output_rows.size()) {
			return false;
		}
		const duckdb::idx_t remaining = output_rows.size() - output_offset;
		const duckdb::idx_t count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto &entry = output_rows[output_offset + row];
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(1, row, duckdb::Value::DOUBLE(entry.mkt_share));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

	//! Fused-scan hooks (disabled in production, see Q8FusedScansEnabled): drain the bound input through the
	//! regular chunk path.
	void DrainBoundInput(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data, Q8InputId input_id) {
		auto source = CreateNativeChunkSource(context, bind_data);
		while (source->Next()) {
			ConsumeInput(static_cast<duckdb::idx_t>(input_id), source->Chunk());
		}
	}

private:
	Q8Parameters params;
	bool has_region_nations = false;
	q8::Tables tables;
	q8::YearVolumes volume_by_year {};
	std::vector<int64_t> scratch_keys;
	std::vector<q8::OrderSlot> scratch_orders;
	std::vector<Q8OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

bool Q8FusedScansEnabled() {
	return false;
}

bool Q8LineitemFusedScanEnabled() {
	return Q8FusedScansEnabled();
}

void Q8RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	static_cast<Q8Kernel &>(kernel).DrainBoundInput(context, bind_data, Q8InputId::CUSTOMER);
}

void Q8RunFusedOrdersScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	static_cast<Q8Kernel &>(kernel).DrainBoundInput(context, bind_data, Q8InputId::ORDERS);
}

void Q8RunFusedLineitemScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	static_cast<Q8Kernel &>(kernel).DrainBoundInput(context, bind_data, Q8InputId::LINEITEM);
}

duckdb::unique_ptr<KernelInstance> CreateQ8Kernel(const Q8Parameters &params) {
	return duckdb::make_uniq<Q8Kernel>(params);
}

} // namespace accelerator_codegen
