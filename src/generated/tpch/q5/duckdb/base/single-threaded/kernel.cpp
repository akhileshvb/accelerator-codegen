#include "tpch/q5/duckdb/base/query.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int32_t kAbsentNation = -1;
constexpr int64_t kAbsentCustomer = 0;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int32_t kMaxNationSlots = 25;

#if defined(__SIZEOF_INT128__)
using q5_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q5_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q5_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q5_acc_t>(extendedprice_raw) * static_cast<q5_acc_t>(100 - discount_raw);
}
#else
using q5_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q5_acc_t &value) {
	return value;
}

void AddRevenue(q5_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q5 revenue");
	}
}
#endif

struct Q5RuntimeConfig {
	bool dense_state = true;
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
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

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

struct Q5OutputRow {
	std::string nation_name;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q5OutputRow &lhs, const Q5OutputRow &rhs) {
	return duckdb::Hugeint::GreaterThan(lhs.revenue, rhs.revenue);
}

template <typename ValueT, ValueT AbsentValue>
class DenseKeyStore {
public:
	explicit DenseKeyStore(int64_t default_capacity_p) : default_capacity(default_capacity_p) {
	}

	void WarmInitialize() {
		if (initialized) {
			return;
		}
		values.assign(static_cast<size_t>(default_capacity), AbsentValue);
		capacity = default_capacity;
		initialized = true;
	}

	void Store(int64_t key, ValueT value) {
		if (key < 0) {
			overflow[key] = value;
			return;
		}
		if (key >= capacity) {
			if (key > 1'000'000'000) {
				overflow[key] = value;
				return;
			}
			EnsureCapacity(key + 1);
		}
		values[static_cast<size_t>(key)] = value;
	}

	ValueT Load(int64_t key) const {
		if (key < 0) {
			const auto it = overflow.find(key);
			return it == overflow.end() ? AbsentValue : it->second;
		}
		if (key >= 0 && key < capacity) {
			return values[static_cast<size_t>(key)];
		}
		const auto it = overflow.find(key);
		return it == overflow.end() ? AbsentValue : it->second;
	}

	inline ValueT LoadNonNegUnchecked(uint64_t key) const {
		return values[key];
	}

	inline void StoreNonNegUnchecked(uint64_t key, ValueT value) {
		values[key] = value;
	}

	bool CanProbeUnchecked(int64_t key) const {
		return key >= 0 && key < capacity;
	}

private:
	void EnsureCapacity(int64_t new_capacity) {
		if (new_capacity <= capacity) {
			return;
		}
		int64_t target = new_capacity;
		if (capacity > 0) {
			const int64_t doubled = capacity > (std::numeric_limits<int64_t>::max() / 2) ? std::numeric_limits<int64_t>::max()
			                                                                               : capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		values.resize(static_cast<size_t>(target), AbsentValue);
		capacity = target;
	}

	int64_t default_capacity;
	bool initialized = false;
	int64_t capacity = 0;
	std::vector<ValueT> values;
	std::unordered_map<int64_t, ValueT> overflow;
};

class Q5Kernel final : public KernelInstance {
public:
	explicit Q5Kernel(Q5Parameters params_p, Q5ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(),
	      customer_nations(kDefaultCustomerCapacity), supplier_nations(kDefaultSupplierCapacity),
	      order_customers(kDefaultOrderCapacity) {
		if (config.dense_state) {
			customer_nations.WarmInitialize();
			supplier_nations.WarmInitialize();
			order_customers.WarmInitialize();
			nation_names.resize(kMaxNationSlots);
			qualifying_nations.assign(kMaxNationSlots, false);
			revenue_by_nation.assign(kMaxNationSlots, 0);
			has_revenue_by_nation.assign(kMaxNationSlots, false);
			if (profile) {
				profile->used_dense_state = true;
			}
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q5InputId>(input_id)) {
		case Q5InputId::REGION:
			ConsumeRegion(input);
			return;
		case Q5InputId::NATION:
			ConsumeNation(input);
			return;
		case Q5InputId::CUSTOMER:
			ConsumeCustomer(input);
			return;
		case Q5InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q5InputId::ORDERS:
			ConsumeOrders(input);
			return;
		case Q5InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		default:
			throw duckdb::InternalException("Q5 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		for (int32_t nationkey = 0; nationkey < kMaxNationSlots; nationkey++) {
			if (!qualifying_nations[static_cast<size_t>(nationkey)] || !has_revenue_by_nation[static_cast<size_t>(nationkey)]) {
				continue;
			}
			Q5OutputRow row;
			row.nation_name = nation_names[static_cast<size_t>(nationkey)];
			row.revenue = FromAccumulator(revenue_by_nation[static_cast<size_t>(nationkey)]);
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		output_offset = 0;
		if (profile) {
			profile->group_count = output_rows.size();
			profile->output_rows = output_rows.size();
		}
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
			output.SetValue(0, row, duckdb::Value(entry.nation_name));
			output.SetValue(1, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void ConsumeRegion(duckdb::DataChunk &input) {
		auto &regionkey_vector = input.data[Q5RegionChunkLayout::REGIONKEY_INDEX];
		auto &name_vector = input.data[Q5RegionChunkLayout::NAME_INDEX];
		duckdb::UnifiedVectorFormat regionkey_format;
		duckdb::UnifiedVectorFormat name_format;
		regionkey_vector.ToUnifiedFormat(input.size(), regionkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		auto regionkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(regionkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->region_rows_scanned += 1;
			}
			const auto regionkey_index = regionkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!regionkey_format.validity.AllValid() && !regionkey_format.validity.RowIsValidUnsafe(regionkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			if (!StringEquals(name_value, params.region_name)) {
				continue;
			}
			target_regionkey = regionkeys[regionkey_index];
			has_target_regionkey = true;
		}
	}

	void ConsumeNation(duckdb::DataChunk &input) {
		if (!has_target_regionkey) {
			return;
		}
		auto &nationkey_vector = input.data[Q5NationChunkLayout::NATIONKEY_INDEX];
		auto &name_vector = input.data[Q5NationChunkLayout::NAME_INDEX];
		auto &regionkey_vector = input.data[Q5NationChunkLayout::REGIONKEY_INDEX];
		if (IsFlatAllValid(nationkey_vector) && IsFlatAllValid(name_vector) && IsFlatAllValid(regionkey_vector)) {
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			auto regionkeys = duckdb::FlatVector::GetData<int32_t>(regionkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->nation_rows_scanned += 1;
				}
				if (regionkeys[row] != target_regionkey) {
					continue;
				}
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots) {
					continue;
				}
				qualifying_nations[static_cast<size_t>(nationkey)] = true;
				nation_names[static_cast<size_t>(nationkey)] =
				    std::string(names[row].GetData(), names[row].GetData() + names[row].GetSize());
			}
			return;
		}
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat regionkey_format;
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		regionkey_vector.ToUnifiedFormat(input.size(), regionkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		auto regionkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(regionkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->nation_rows_scanned += 1;
			}
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto regionkey_index = regionkey_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!regionkey_format.validity.AllValid() &&
			     !regionkey_format.validity.RowIsValidUnsafe(regionkey_index))) {
				continue;
			}
			if (regionkeys[regionkey_index] != target_regionkey) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			qualifying_nations[static_cast<size_t>(nationkey)] = true;
			nation_names[static_cast<size_t>(nationkey)] =
			    std::string(name_value.GetData(), name_value.GetData() + name_value.GetSize());
		}
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &custkey_vector = input.data[Q5CustomerChunkLayout::CUSTKEY_INDEX];
		auto &nationkey_vector = input.data[Q5CustomerChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->customer_rows_scanned += 1;
				}
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots || !qualifying_nations[static_cast<size_t>(nationkey)]) {
					continue;
				}
				customer_nations.Store(custkeys[row], nationkey);
			}
			return;
		}
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat nationkey_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->customer_rows_scanned += 1;
			}
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots || !qualifying_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			customer_nations.Store(custkeys[custkey_index], nationkey);
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &suppkey_vector = input.data[Q5SupplierChunkLayout::SUPPKEY_INDEX];
		auto &nationkey_vector = input.data[Q5SupplierChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->supplier_rows_scanned += 1;
				}
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots || !qualifying_nations[static_cast<size_t>(nationkey)]) {
					continue;
				}
				supplier_nations.Store(suppkeys[row], nationkey);
			}
			return;
		}
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat nationkey_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->supplier_rows_scanned += 1;
			}
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots || !qualifying_nations[static_cast<size_t>(nationkey)]) {
				continue;
			}
			supplier_nations.Store(suppkeys[suppkey_index], nationkey);
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &orderkey_vector = input.data[Q5OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q5OrdersChunkLayout::CUSTKEY_INDEX];
		auto &orderdate_vector = input.data[Q5OrdersChunkLayout::ORDERDATE_INDEX];
		if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector) && IsFlatAllValid(orderdate_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto orderdates = duckdb::FlatVector::GetData<duckdb::date_t>(orderdate_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				const auto orderdate = orderdates[row];
				if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
					continue;
				}
				const auto custkey = custkeys[row];
				if (customer_nations.Load(custkey) == kAbsentNation) {
					continue;
				}
				order_customers.Store(orderkeys[row], custkey);
			}
			return;
		}
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat orderdate_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		orderdate_vector.ToUnifiedFormat(input.size(), orderdate_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto orderdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(orderdate_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->orders_rows_scanned += 1;
			}
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto orderdate_index = orderdate_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!orderdate_format.validity.AllValid() && !orderdate_format.validity.RowIsValidUnsafe(orderdate_index))) {
				continue;
			}
			const auto orderdate = orderdates[orderdate_index];
			if (!(orderdate >= params.start_date && orderdate < params.end_date)) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			if (customer_nations.Load(custkey) == kAbsentNation) {
				continue;
			}
			order_customers.Store(orderkeys[orderkey_index], custkey);
		}
	}

	bool ProcessLineitemRow(int64_t orderkey, int64_t suppkey, int64_t extendedprice_raw, int64_t discount_raw) {
		const auto custkey = order_customers.Load(orderkey);
		if (custkey == kAbsentCustomer) {
			return false;
		}
		const auto cust_nation = customer_nations.Load(custkey);
		const auto supp_nation = supplier_nations.Load(suppkey);
		if (cust_nation == kAbsentNation || supp_nation == kAbsentNation || cust_nation != supp_nation) {
			return false;
		}
		if (cust_nation < 0 || cust_nation >= kMaxNationSlots) {
			return false;
		}
		if (profile) {
			profile->lineitem_rows_matched += 1;
		}
		AddRevenue(revenue_by_nation[static_cast<size_t>(cust_nation)], extendedprice_raw, discount_raw);
		has_revenue_by_nation[static_cast<size_t>(cust_nation)] = true;
		return true;
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		if (profile) {
			profile->lineitem_rows_scanned += input.size();
		}
		auto &orderkey_vector = input.data[Q5LineitemChunkLayout::ORDERKEY_INDEX];
		auto &suppkey_vector = input.data[Q5LineitemChunkLayout::SUPPKEY_INDEX];
		auto &extendedprice_vector = input.data[Q5LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q5LineitemChunkLayout::DISCOUNT_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(extendedprice_vector) && IsFlatAllValid(discount_vector)) {
			if (profile) {
				profile->lineitem_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				(void)ProcessLineitemRow(orderkeys[row], suppkeys[row], extendedprices[row], discounts[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector)) {
			if (profile) {
				profile->lineitem_dict_fast_rows += input.size();
			}
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				(void)ProcessLineitemRow(orderkeys[orderkey_index], suppkeys[suppkey_index],
				                         extendedprices[extendedprice_index], discounts[discount_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			(void)ProcessLineitemRow(orderkeys[orderkey_index], suppkeys[suppkey_index],
			                         extendedprices[extendedprice_index], discounts[discount_index]);
		}
	}

	bool has_qualifying_nations() const {
		for (const auto flag : qualifying_nations) {
			if (flag) {
				return true;
			}
		}
		return false;
	}

	Q5Parameters params;
	Q5ProfileCounters *profile;
	Q5RuntimeConfig config;
	int32_t target_regionkey = 0;
	bool has_target_regionkey = false;
	std::vector<bool> qualifying_nations;
	std::vector<std::string> nation_names;
	DenseKeyStore<int32_t, kAbsentNation> customer_nations;
	DenseKeyStore<int32_t, kAbsentNation> supplier_nations;
	DenseKeyStore<int64_t, kAbsentCustomer> order_customers;
	std::vector<q5_acc_t> revenue_by_nation;
	std::vector<bool> has_revenue_by_nation;
	std::vector<Q5OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ5Kernel(const Q5Parameters &params) {
	return duckdb::make_uniq<Q5Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ5ProfilingKernel(const Q5Parameters &params, Q5ProfileCounters *profile) {
	return duckdb::make_uniq<Q5Kernel>(params, profile);
}

} // namespace accelerator_codegen
