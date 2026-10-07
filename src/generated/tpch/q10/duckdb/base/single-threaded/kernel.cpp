#include "tpch/q10/duckdb/base/query.hpp"
#include "tpch/q10/duckdb/base/q10_common.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;

struct Q10OutputRow {
	int64_t custkey = 0;
	std::string name;
	duckdb::hugeint_t revenue;
	duckdb::hugeint_t acctbal;
	std::string nation_name;
	std::string address;
	std::string phone;
	std::string comment;
};

class Q10Kernel final : public KernelInstance {
public:
	explicit Q10Kernel(Q10Parameters params_p, Q10ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p) {
		order_parts.resize(1);
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q10InputId>(input_id)) {
		case Q10InputId::NATION:
			q10::ConsumeNationChunk(input, nation_names);
			return;
		case Q10InputId::ORDERS:
			if (profile) {
				profile->orders_rows_scanned += input.size();
			}
			q10::CollectOrdersChunk(input, order_parts[0]);
			return;
		case Q10InputId::LINEITEM:
			EnsureIndex();
			if (profile) {
				profile->lineitem_rows_scanned += input.size();
			}
			if (index.Empty()) {
				return;
			}
			q10::AccumulateLineitemChunk(input, index, acc);
			return;
		case Q10InputId::CUSTOMER:
			if (fused_customer_scanned) {
				throw duckdb::InternalException("Q10 customer already consumed via fused scan-consumer");
			}
			EnsureIndex();
			if (index.Empty()) {
				return;
			}
			q10::ProcessCustomerChunk(input, ScanInput(), heap);
			return;
		default:
			throw duckdb::InternalException("Q10 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		std::vector<q10::TopK *> heaps {&heap};
		auto rows = q10::FinishTopK(std::move(heaps));
		output_rows.clear();
		output_rows.reserve(rows.size());
		for (auto &row : rows) {
			Q10OutputRow out;
			out.custkey = row.custkey;
			out.name = std::move(row.name);
			out.revenue = q10::ToHugeint(row.revenue);
			out.acctbal = duckdb::hugeint_t(row.acctbal_raw);
			out.nation_name = nation_names[static_cast<size_t>(row.nationkey)];
			out.address = std::move(row.address);
			out.phone = std::move(row.phone);
			out.comment = std::move(row.comment);
			output_rows.push_back(std::move(out));
		}
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.acctbal, 15, 2));
			output.SetValue(4, row, duckdb::Value(entry.nation_name));
			output.SetValue(5, row, duckdb::Value(entry.address));
			output.SetValue(6, row, duckdb::Value(entry.phone));
			output.SetValue(7, row, duckdb::Value(entry.comment));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

	void RunFusedCustomerScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
		if (fused_customer_scanned) {
			return;
		}
		EnsureIndex();
		fused_customer_scanned = true;
		if (index.Empty()) {
			return;
		}
		if (profile) {
			profile->used_fused_customer = true;
		}
		const auto input = ScanInput();
		auto consumer = [&](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
		                    duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
		                    duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
			if (profile) {
				profile->customer_fused_rows_scanned += count;
			}
			q10::ProcessCustomerVector(transaction, scan_state, row_group, sel, count, input, heap);
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		};
		const auto floor = q10::ComputeFloor(index, acc);
		heap.SetFloor(floor);
		if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
			throw duckdb::InterruptException();
		}
		if (floor.active && heap.Rows().size() < q10::kResultLimit) {
			// Some of the top-ranked customers have no customer row / nation: rank without the floor.
			heap = q10::TopK();
			if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
				throw duckdb::InterruptException();
			}
		}
	}

	bool FusedCustomerScanned() const {
		return fused_customer_scanned;
	}

private:
	//! Orders are fully consumed before the first lineitem/customer input: freeze them into the index.
	void EnsureIndex() {
		if (index_ready) {
			return;
		}
		index.Build(order_parts);
		order_parts.clear();
		acc.Init(index.CustomerCount());
		index_ready = true;
	}

	q10::CustomerScanInput ScanInput() const {
		q10::CustomerScanInput input;
		input.index = &index;
		input.acc = &acc;
		input.nation_names = &nation_names;
		return input;
	}

	Q10Parameters params;
	Q10ProfileCounters *profile;
	std::array<std::string, q10::kMaxNationSlots> nation_names;
	std::vector<std::vector<q10::OrderPair>> order_parts;
	q10::OrderCustomerIndex index;
	q10::Accumulator acc;
	q10::TopK heap;
	bool index_ready = false;
	bool fused_customer_scanned = false;
	std::vector<Q10OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

bool Q10FusedScansEnabled() {
	return true;
}

void Q10RunFusedCustomerScan(duckdb::ClientContext &context, KernelInstance &kernel, const duckdb::FunctionData &bind_data) {
	auto &q10_kernel = static_cast<Q10Kernel &>(kernel);
	q10_kernel.RunFusedCustomerScan(context, bind_data);
}

duckdb::unique_ptr<KernelInstance> CreateQ10Kernel(const Q10Parameters &params) {
	return duckdb::make_uniq<Q10Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ10ProfilingKernel(const Q10Parameters &params, Q10ProfileCounters *profile) {
	return duckdb::make_uniq<Q10Kernel>(params, profile);
}

} // namespace accelerator_codegen
