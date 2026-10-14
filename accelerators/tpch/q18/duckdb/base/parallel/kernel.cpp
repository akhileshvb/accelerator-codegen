#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "backends/duckdb/api/parallel/parallel_kernel.hpp"
#include "backends/duckdb/api/parallel/parallel_runtime.hpp"
#include "tpch/q18/duckdb/base/agg.hpp"
#include "tpch/q18/duckdb/base/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int64_t kDefaultCustomerCapacity = 150001;

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlat(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
}

struct OrderMetadata {
	int64_t custkey = 0;
	duckdb::date_t orderdate;
	duckdb::hugeint_t totalprice;
};

struct QualifyingOrderRow {
	int64_t orderkey = 0;
	OrderMetadata metadata;
};

struct Q18OutputRow {
	std::string name;
	int64_t custkey = 0;
	int64_t orderkey = 0;
	duckdb::date_t orderdate;
	duckdb::hugeint_t totalprice;
	duckdb::hugeint_t quantity_sum;
};

bool BetterOutputRow(const Q18OutputRow &lhs, const Q18OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.totalprice, rhs.totalprice)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.totalprice, lhs.totalprice)) {
		return false;
	}
	return lhs.orderdate < rhs.orderdate;
}

//! Lineitem-phase result shared (read-only) by the orders phase.
struct Q18FrozenState {
	q18agg::Resolved resolved;
	q18agg::KeyBitmap qualifying;
	bool has_qualifying_orders = false;

	bool IsQualifyingOrder(int64_t orderkey) const {
		return qualifying.Contains(orderkey);
	}

	int64_t OrderQuantity(int64_t orderkey) const {
		const auto it = resolved.totals.find(orderkey);
		return it == resolved.totals.end() ? 0 : it->second;
	}
};

struct Q18OrdersWorkerState {
	std::vector<QualifyingOrderRow> orders;
	std::vector<uint8_t> needed_custkeys;

	void MarkCustkey(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(custkey) + 1;
		if (required > needed_custkeys.size()) {
			size_t target = required;
			if (!needed_custkeys.empty()) {
				const size_t doubled = needed_custkeys.size() * 2;
				if (doubled > target) {
					target = doubled;
				}
			} else {
				target = std::max(target, static_cast<size_t>(kDefaultCustomerCapacity));
			}
			needed_custkeys.resize(target, 0);
		}
		needed_custkeys[static_cast<size_t>(custkey)] = 1;
	}
};

struct Q18ParallelProfile {
	int64_t lineitem_scan_ns = 0;
	int64_t resolve_ns = 0;
	int64_t orders_scan_ns = 0;
	int64_t orders_merge_ns = 0;
	int64_t customer_ns = 0;
	int64_t finalize_ns = 0;
	size_t worker_count = 0;
	size_t order_capacity = 0;
	size_t qualifying_orders = 0;
};

//! Aggregate one lineitem chunk into the shared totals (atomic adds when several workers run).
template <bool kAtomic>
void AccumulateLineitemChunk(duckdb::DataChunk &chunk, q18agg::TotalsArray &totals, const q18agg::Config &cfg,
                             q18agg::WorkerAgg &worker) {
	auto &orderkey_vector = chunk.data[Q18LineitemChunkLayout::ORDERKEY_INDEX];
	auto &quantity_vector = chunk.data[Q18LineitemChunkLayout::QUANTITY_INDEX];
	const auto count = chunk.size();

	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(quantity_vector)) {
		q18agg::AggregateChunk<kAtomic, false>(totals, cfg, worker, duckdb::FlatVector::GetData<int64_t>(orderkey_vector),
		                                       duckdb::FlatVector::GetData<int64_t>(quantity_vector), count);
		return;
	}

	// Non-flat / nullable input: compact the valid rows, then take the same aggregation core.
	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat quantity_format;
	orderkey_vector.ToUnifiedFormat(count, orderkey_format);
	quantity_vector.ToUnifiedFormat(count, quantity_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
	int64_t key_buf[STANDARD_VECTOR_SIZE];
	int64_t qty_buf[STANDARD_VECTOR_SIZE];
	duckdb::idx_t valid = 0;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto quantity_index = quantity_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
			continue;
		}
		key_buf[valid] = orderkeys[orderkey_index];
		qty_buf[valid] = quantities[quantity_index];
		valid++;
	}
	q18agg::AggregateChunk<kAtomic, false>(totals, cfg, worker, key_buf, qty_buf, valid);
}

void AccumulateOrdersChunk(duckdb::DataChunk &chunk, const Q18FrozenState &state, Q18OrdersWorkerState &worker) {
	if (!state.has_qualifying_orders) {
		return;
	}

	auto &orderkey_vector = chunk.data[Q18OrdersChunkLayout::ORDERKEY_INDEX];
	auto &custkey_vector = chunk.data[Q18OrdersChunkLayout::CUSTKEY_INDEX];
	auto &orderdate_vector = chunk.data[Q18OrdersChunkLayout::ORDERDATE_INDEX];
	auto &totalprice_vector = chunk.data[Q18OrdersChunkLayout::TOTALPRICE_INDEX];
	if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector) &&
	    IsFlatAllValid(totalprice_vector)) {
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto orderdates = duckdb::FlatVector::GetData<int32_t>(orderdate_vector);
		auto totalprices = duckdb::FlatVector::GetData<int64_t>(totalprice_vector);
		uint32_t hits[STANDARD_VECTOR_SIZE + simd::kSelSlack];
		const auto hit_count = state.qualifying.Select(orderkeys, chunk.size(), hits);
		for (size_t h = 0; h < hit_count; h++) {
			const auto row = hits[h];
			const auto orderkey = orderkeys[row];
			const auto custkey = custkeys[row];
			worker.MarkCustkey(custkey);
			worker.orders.push_back(
			    QualifyingOrderRow {orderkey,
			                        OrderMetadata {custkey, duckdb::date_t(orderdates[row]), duckdb::hugeint_t(totalprices[row])}});
		}
		return;
	}

	if (IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(custkey_vector) &&
	    IsDictionaryOverFlat(orderdate_vector) && IsDictionaryOverFlat(totalprice_vector)) {
		auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto &orderdate_sel = duckdb::DictionaryVector::SelVector(orderdate_vector);
		auto &totalprice_sel = duckdb::DictionaryVector::SelVector(totalprice_vector);
		auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto orderdates = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(orderdate_vector));
		auto totalprices = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(totalprice_vector));
		auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		auto &orderdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderdate_vector));
		auto &totalprice_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(totalprice_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto orderkey_index = orderkey_sel.get_index(row);
			const auto custkey_index = custkey_sel.get_index(row);
			const auto orderdate_index = orderdate_sel.get_index(row);
			const auto totalprice_index = totalprice_sel.get_index(row);
			if (!orderkey_validity.RowIsValid(orderkey_index) || !custkey_validity.RowIsValid(custkey_index) ||
			    !orderdate_validity.RowIsValid(orderdate_index) || !totalprice_validity.RowIsValid(totalprice_index)) {
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!state.IsQualifyingOrder(orderkey)) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			worker.MarkCustkey(custkey);
			worker.orders.push_back(QualifyingOrderRow {
			    orderkey,
			    OrderMetadata {custkey, duckdb::date_t(orderdates[orderdate_index]),
			                   duckdb::hugeint_t(totalprices[totalprice_index])}});
		}
		return;
	}

	duckdb::UnifiedVectorFormat orderkey_format;
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat orderdate_format;
	duckdb::UnifiedVectorFormat totalprice_format;
	orderkey_vector.ToUnifiedFormat(chunk.size(), orderkey_format);
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	orderdate_vector.ToUnifiedFormat(chunk.size(), orderdate_format);
	totalprice_vector.ToUnifiedFormat(chunk.size(), totalprice_format);
	auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
	auto totalprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(totalprice_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto orderkey_index = orderkey_format.sel->get_index(row);
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto orderdate_index = orderdate_format.sel->get_index(row);
		const auto totalprice_index = totalprice_format.sel->get_index(row);
		if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
		    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
		    (!totalprice_format.validity.AllValid() &&
		     !totalprice_format.validity.RowIsValidUnsafe(totalprice_index))) {
			continue;
		}
		const auto orderkey = orderkeys[orderkey_index];
		if (!state.IsQualifyingOrder(orderkey)) {
			continue;
		}
		const auto custkey = custkeys[custkey_index];
		worker.MarkCustkey(custkey);
		worker.orders.push_back(QualifyingOrderRow {
		    orderkey,
		    OrderMetadata {custkey, duckdb::date_t(orderdates[orderdate_index]),
		                   duckdb::hugeint_t(totalprices[totalprice_index])}});
	}
}

void MergeCustkeyBitmap(std::vector<uint8_t> &global, const std::vector<uint8_t> &local) {
	if (local.size() > global.size()) {
		global.resize(local.size(), 0);
	}
	for (size_t index = 0; index < local.size(); index++) {
		if (local[index] != 0) {
			global[index] = 1;
		}
	}
}

void BuildCustomerState(duckdb::ClientContext &context, const duckdb::FunctionData &customer_bind,
                        duckdb::idx_t worker_count, const std::vector<uint8_t> &needed_custkeys,
                        std::vector<std::string> &customer_names) {
	if (needed_custkeys.empty()) {
		return;
	}
	customer_names.resize(needed_custkeys.size());

	// Workers write disjoint customer_names slots (one per custkey), so no synchronisation is needed.
	RunParallelChunkScan(context, customer_bind, worker_count, [&](duckdb::idx_t, duckdb::DataChunk &chunk) {
		auto &custkey_vector = chunk.data[Q18CustomerChunkLayout::CUSTKEY_INDEX];
		auto &name_vector = chunk.data[Q18CustomerChunkLayout::NAME_INDEX];

		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(name_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto custkey = custkeys[row];
				if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
				    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				const auto &name_value = names[row];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
			}
			return;
		}

		if (IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(name_vector)) {
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto &name_sel = duckdb::DictionaryVector::SelVector(name_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(name_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			auto &name_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(name_vector));
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto custkey_index = custkey_sel.get_index(row);
				const auto name_index = name_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index) || !name_validity.RowIsValid(name_index)) {
					continue;
				}
				const auto custkey = custkeys[custkey_index];
				if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
				    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
					continue;
				}
				const auto &name_value = names[name_index];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat name_format;
		chunk.data[Q18CustomerChunkLayout::CUSTKEY_INDEX].ToUnifiedFormat(chunk.size(), custkey_format);
		chunk.data[Q18CustomerChunkLayout::NAME_INDEX].ToUnifiedFormat(chunk.size(), name_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (custkey < 0 || static_cast<size_t>(custkey) >= needed_custkeys.size() ||
			    needed_custkeys[static_cast<size_t>(custkey)] == 0) {
				continue;
			}
			const auto &name_value = names[name_index];
			customer_names[static_cast<size_t>(custkey)] =
			    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
		}
	});
}

class Q18ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q18ParallelKernel(Q18Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q18 parallel expects bound lineitem, orders, customer scans");
		}

		using clock = std::chrono::steady_clock;
		Q18ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		const auto order_capacity = EstimateTpchDenseOrderCapacity(context, *bound_inputs[1]);
		const auto capacity = std::max<int64_t>(order_capacity, kDefaultOrderCapacity);
		profile.order_capacity = static_cast<size_t>(capacity);

		// Phase 1: shared uint32 totals array; adds are atomic when several workers scan lineitem.
		// Out-of-contract rows (negative / huge quantity, keys beyond the array) are kept exactly in
		// per-worker exceptional lists and resolved below.
		const int64_t threshold_raw = params.quantity_threshold * 100;
		const auto cfg = q18agg::Config::Make(threshold_raw);
		q18agg::TotalsArray totals;
		totals.Init(static_cast<size_t>(capacity));
		std::vector<CacheLinePadded<q18agg::WorkerAgg>> lineitem_workers(worker_count);

		const auto lineitem_t0 = clock::now();
		if (worker_count == 1) {
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     AccumulateLineitemChunk<false>(chunk, totals, cfg,
				                                                    lineitem_workers[static_cast<size_t>(worker_id)].value);
			                     });
		} else {
			RunParallelChunkScan(context, *bound_inputs[0], worker_count,
			                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
				                     AccumulateLineitemChunk<true>(chunk, totals, cfg,
				                                                   lineitem_workers[static_cast<size_t>(worker_id)].value);
			                     });
		}
		profile.lineitem_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - lineitem_t0).count();

		const auto resolve_t0 = clock::now();
		Q18FrozenState frozen;
		{
			std::vector<q18agg::WorkerAgg *> worker_ptrs;
			for (auto &w : lineitem_workers) {
				worker_ptrs.push_back(&w.value);
			}
			frozen.resolved = q18agg::Resolve(totals, threshold_raw, worker_ptrs);
		}
		frozen.qualifying.Build(frozen.resolved.keys);
		frozen.has_qualifying_orders = !frozen.resolved.keys.empty();
		profile.qualifying_orders = frozen.resolved.keys.size();
		lineitem_workers.clear();
		lineitem_workers.shrink_to_fit();
		profile.resolve_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - resolve_t0).count();

		std::vector<CacheLinePadded<Q18OrdersWorkerState>> order_workers(worker_count);
		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     AccumulateOrdersChunk(chunk, frozen, order_workers[static_cast<size_t>(worker_id)].value);
		                     });
		profile.orders_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		const auto orders_merge_t0 = clock::now();
		std::vector<QualifyingOrderRow> orders;
		std::vector<uint8_t> needed_custkeys;
		for (const auto &worker : order_workers) {
			orders.insert(orders.end(), worker.value.orders.begin(), worker.value.orders.end());
			MergeCustkeyBitmap(needed_custkeys, worker.value.needed_custkeys);
		}
		order_workers.clear();
		order_workers.shrink_to_fit();
		profile.orders_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_merge_t0).count();

		const auto customer_t0 = clock::now();
		std::vector<std::string> customer_names;
		BuildCustomerState(context, *bound_inputs[2], worker_count, needed_custkeys, customer_names);
		profile.customer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - customer_t0).count();

		const auto finalize_t0 = clock::now();
		output_rows.clear();
		output_rows.reserve(orders.size());
		for (const auto &entry : orders) {
			const auto &metadata = entry.metadata;
			if (metadata.custkey < 0 || static_cast<size_t>(metadata.custkey) >= customer_names.size()) {
				continue;
			}
			const auto &name = customer_names[static_cast<size_t>(metadata.custkey)];
			if (name.empty()) {
				continue;
			}
			Q18OutputRow row;
			row.name = name;
			row.custkey = metadata.custkey;
			row.orderkey = entry.orderkey;
			row.orderdate = metadata.orderdate;
			row.totalprice = metadata.totalprice;
			row.quantity_sum = duckdb::hugeint_t(frozen.OrderQuantity(entry.orderkey));
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.finalize_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - finalize_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			std::cerr << "q18_parallel_profile lineitem_scan_ns=" << profile.lineitem_scan_ns
			          << " resolve_ns=" << profile.resolve_ns << " orders_scan_ns=" << profile.orders_scan_ns
			          << " orders_merge_ns=" << profile.orders_merge_ns << " customer_ns=" << profile.customer_ns
			          << " finalize_ns=" << profile.finalize_ns << " worker_count=" << profile.worker_count
			          << " order_capacity=" << profile.order_capacity
			          << " qualifying_orders=" << profile.qualifying_orders << "\n";
		}
	}

	void Finalize() override {
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
			output.SetValue(0, row, duckdb::Value(entry.name));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.custkey));
			output.SetValue(2, row, duckdb::Value::BIGINT(entry.orderkey));
			output.SetValue(3, row, duckdb::Value::DATE(entry.orderdate));
			output.SetValue(4, row, duckdb::Value::DECIMAL(entry.totalprice, 15, 2));
			output.SetValue(5, row, duckdb::Value::DECIMAL(entry.quantity_sum, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q18Parameters params;
	std::vector<Q18OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ18ParallelKernel(const Q18Parameters &params) {
	return duckdb::make_uniq<Q18ParallelKernel>(params);
}

} // namespace accelerator_codegen
