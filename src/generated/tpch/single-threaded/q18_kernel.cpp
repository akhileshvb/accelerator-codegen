#include "accelerator_codegen/tpch_q18.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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

struct Q18RuntimeConfig {
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
	bool orders_flat_fast = true;
	bool orders_dict_fast = true;
	bool customer_flat_fast = true;
	bool customer_dict_fast = true;
};

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

class Q18Kernel final : public KernelInstance {
public:
	explicit Q18Kernel(Q18Parameters params_p, Q18ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config() {
		quantity_threshold_raw = params.quantity_threshold * 100;
		order_quantity_totals.assign(static_cast<size_t>(kDefaultOrderCapacity), 0);
		order_qualifying.assign(static_cast<size_t>(kDefaultOrderCapacity), 0);
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q18InputId>(input_id)) {
		case Q18InputId::LINEITEM_AGG:
			ConsumeLineitemAgg(input);
			return;
		case Q18InputId::ORDERS:
			EnsureQualifyingOrdersReady();
			ConsumeOrders(input);
			return;
		case Q18InputId::CUSTOMER:
			ConsumeCustomer(input);
			return;
		default:
			throw duckdb::InternalException("Q18 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		output_rows.reserve(orders.size());
		const auto order_capacity = static_cast<int64_t>(order_quantity_totals.size());
		for (const auto &entry : orders) {
			const auto orderkey = entry.first;
			if (orderkey < 0 || orderkey >= order_capacity) {
				continue;
			}
			if (!IsQualifyingOrder(orderkey)) {
				continue;
			}
			const auto &metadata = entry.second;
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
			row.orderkey = orderkey;
			row.orderdate = metadata.orderdate;
			row.totalprice = metadata.totalprice;
			row.quantity_sum = duckdb::hugeint_t(order_quantity_totals[static_cast<size_t>(orderkey)]);
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
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
	void EnsureOrderCapacity(int64_t orderkey) {
		if (orderkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(orderkey) + 1;
		if (required <= order_quantity_totals.size()) {
			return;
		}
		size_t target = required;
		if (!order_quantity_totals.empty()) {
			const size_t doubled = order_quantity_totals.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultOrderCapacity));
		}
		order_quantity_totals.resize(target, 0);
		order_qualifying.resize(target, 0);
	}

	void EnsureCustomerCapacity(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(custkey) + 1;
		if (required <= customer_names.size()) {
			return;
		}
		size_t target = required;
		if (!customer_names.empty()) {
			const size_t doubled = customer_names.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultCustomerCapacity));
		}
		customer_names.resize(target);
	}

	void EnsureQualifyingOrdersReady() {
		if (qualifying_orders_ready) {
			return;
		}
		for (int64_t orderkey = 0; orderkey < static_cast<int64_t>(order_quantity_totals.size()); orderkey++) {
			if (order_quantity_totals[static_cast<size_t>(orderkey)] > quantity_threshold_raw) {
				order_qualifying[static_cast<size_t>(orderkey)] = 1;
				has_qualifying_orders = true;
			}
		}
		qualifying_orders_ready = true;
	}

	bool IsQualifyingOrder(int64_t orderkey) const {
		if (orderkey < 0 || static_cast<size_t>(orderkey) >= order_qualifying.size()) {
			return false;
		}
		return order_qualifying[static_cast<size_t>(orderkey)] != 0;
	}

	void AddOrderQuantity(int64_t orderkey, int64_t quantity_raw) {
		EnsureOrderCapacity(orderkey);
		order_quantity_totals[static_cast<size_t>(orderkey)] += quantity_raw;
	}

	void ConsumeLineitemAgg(duckdb::DataChunk &input) {
		if (profile) {
			profile->lineitem_rows_scanned += input.size();
		}
		auto &orderkey_vector = input.data[Q18LineitemChunkLayout::ORDERKEY_INDEX];
		auto &quantity_vector = input.data[Q18LineitemChunkLayout::QUANTITY_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(quantity_vector)) {
			if (profile) {
				profile->lineitem_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto quantities = duckdb::FlatVector::GetData<int64_t>(quantity_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				AddOrderQuantity(orderkeys[row], quantities[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(quantity_vector)) {
			if (profile) {
				profile->lineitem_dict_fast_rows += input.size();
			}
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &quantity_sel = duckdb::DictionaryVector::SelVector(quantity_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto quantities = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(quantity_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &quantity_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(quantity_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto quantity_index = quantity_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !quantity_validity.RowIsValid(quantity_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				AddOrderQuantity(orderkeys[orderkey_index], quantities[quantity_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			AddOrderQuantity(orderkeys[orderkey_index], quantities[quantity_index]);
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (!has_qualifying_orders) {
			return;
		}
		if (profile) {
			profile->orders_rows_scanned += input.size();
		}
		auto &orderkey_vector = input.data[Q18OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q18OrdersChunkLayout::CUSTKEY_INDEX];
		auto &orderdate_vector = input.data[Q18OrdersChunkLayout::ORDERDATE_INDEX];
		auto &totalprice_vector = input.data[Q18OrdersChunkLayout::TOTALPRICE_INDEX];

		if (config.orders_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) &&
		    IsFlatAllValid(orderdate_vector) && IsFlatAllValid(totalprice_vector)) {
			if (profile) {
				profile->orders_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto orderdates = duckdb::FlatVector::GetData<int32_t>(orderdate_vector);
			auto totalprices = duckdb::FlatVector::GetData<int64_t>(totalprice_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey = orderkeys[row];
				if (!IsQualifyingOrder(orderkey)) {
					continue;
				}
				OrderMetadata metadata;
				metadata.custkey = custkeys[row];
				metadata.orderdate = duckdb::date_t(orderdates[row]);
				metadata.totalprice = duckdb::hugeint_t(totalprices[row]);
				needed_custkeys.insert(metadata.custkey);
				orders.emplace(orderkey, std::move(metadata));
				if (profile) {
					profile->orders_rows_matched += 1;
				}
			}
			return;
		}

		if (config.orders_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(custkey_vector) &&
		    IsDictionaryOverFlat(orderdate_vector) && IsDictionaryOverFlat(totalprice_vector)) {
			if (profile) {
				profile->orders_dict_fast_rows += input.size();
			}
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
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto custkey_index = custkey_sel.get_index(row);
				const auto orderdate_index = orderdate_sel.get_index(row);
				const auto totalprice_index = totalprice_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !custkey_validity.RowIsValid(custkey_index) ||
				    !orderdate_validity.RowIsValid(orderdate_index) || !totalprice_validity.RowIsValid(totalprice_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				const auto orderkey = orderkeys[orderkey_index];
				if (!IsQualifyingOrder(orderkey)) {
					continue;
				}
				OrderMetadata metadata;
				metadata.custkey = custkeys[custkey_index];
				metadata.orderdate = duckdb::date_t(orderdates[orderdate_index]);
				metadata.totalprice = duckdb::hugeint_t(totalprices[totalprice_index]);
				needed_custkeys.insert(metadata.custkey);
				orders.emplace(orderkey, std::move(metadata));
				if (profile) {
					profile->orders_rows_matched += 1;
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat orderdate_format;
		duckdb::UnifiedVectorFormat totalprice_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
		totalprice_vector.ToUnifiedFormat(input.size(), totalprice_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto orderdates = duckdb::UnifiedVectorFormat::GetData<int32_t>(orderdate_format);
		auto totalprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(totalprice_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto orderdate_index = orderdate_format.sel->get_index(row);
			const auto totalprice_index = totalprice_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index)) ||
			    (!totalprice_format.validity.AllValid() &&
			     !totalprice_format.validity.RowIsValidUnsafe(totalprice_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto orderkey = orderkeys[orderkey_index];
			if (!IsQualifyingOrder(orderkey)) {
				continue;
			}
			OrderMetadata metadata;
			metadata.custkey = custkeys[custkey_index];
			metadata.orderdate = duckdb::date_t(orderdates[orderdate_index]);
			metadata.totalprice = duckdb::hugeint_t(totalprices[totalprice_index]);
			needed_custkeys.insert(metadata.custkey);
			orders.emplace(orderkey, std::move(metadata));
			if (profile) {
				profile->orders_rows_matched += 1;
			}
		}
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (needed_custkeys.empty()) {
			return;
		}
		if (profile) {
			profile->customer_rows_scanned += input.size();
		}
		auto &custkey_vector = input.data[Q18CustomerChunkLayout::CUSTKEY_INDEX];
		auto &name_vector = input.data[Q18CustomerChunkLayout::NAME_INDEX];

		if (config.customer_flat_fast && IsFlatAllValid(custkey_vector) && IsFlatAllValid(name_vector)) {
			if (profile) {
				profile->customer_flat_fast_rows += input.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto custkey = custkeys[row];
				if (needed_custkeys.find(custkey) == needed_custkeys.end()) {
					continue;
				}
				EnsureCustomerCapacity(custkey);
				const auto &name_value = names[row];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
				if (profile) {
					profile->customer_rows_matched += 1;
				}
			}
			return;
		}

		if (config.customer_dict_fast && IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(name_vector)) {
			if (profile) {
				profile->customer_dict_fast_rows += input.size();
			}
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto &name_sel = duckdb::DictionaryVector::SelVector(name_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(name_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			auto &name_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(name_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto custkey_index = custkey_sel.get_index(row);
				const auto name_index = name_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index) || !name_validity.RowIsValid(name_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				const auto custkey = custkeys[custkey_index];
				if (needed_custkeys.find(custkey) == needed_custkeys.end()) {
					continue;
				}
				EnsureCustomerCapacity(custkey);
				const auto &name_value = names[name_index];
				customer_names[static_cast<size_t>(custkey)] =
				    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
				if (profile) {
					profile->customer_rows_matched += 1;
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat name_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto names = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (needed_custkeys.find(custkey) == needed_custkeys.end()) {
				continue;
			}
			EnsureCustomerCapacity(custkey);
			const auto &name_value = names[name_index];
			customer_names[static_cast<size_t>(custkey)] =
			    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
			if (profile) {
				profile->customer_rows_matched += 1;
			}
		}
	}

	Q18Parameters params;
	Q18ProfileCounters *profile;
	Q18RuntimeConfig config;
	int64_t quantity_threshold_raw = 0;
	std::vector<int64_t> order_quantity_totals;
	std::vector<uint8_t> order_qualifying;
	std::unordered_set<int64_t> needed_custkeys;
	std::vector<std::string> customer_names;
	std::unordered_map<int64_t, OrderMetadata> orders;
	bool qualifying_orders_ready = false;
	bool has_qualifying_orders = false;
	std::vector<Q18OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ18Kernel(const Q18Parameters &params) {
	return duckdb::make_uniq<Q18Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ18ProfilingKernel(const Q18Parameters &params, Q18ProfileCounters *profile) {
	return duckdb::make_uniq<Q18Kernel>(params, profile);
}

} // namespace accelerator_codegen
