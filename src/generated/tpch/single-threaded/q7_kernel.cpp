#include "accelerator_codegen/tpch_q7.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr uint8_t kRevenueDecimalWidth = 38;
constexpr uint8_t kRevenueDecimalScale = 4;
constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int32_t kAbsentNation = -1;
constexpr uint8_t kAbsentOrderCustClass = 0;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultOrderCapacity = 6000001;
constexpr int32_t kMaxNationSlots = 25;
constexpr int64_t kMinYear = 1995;
constexpr int64_t kMaxYear = 1996;
constexpr int kMaxGroupSlots = 4;

#if defined(__SIZEOF_INT128__)
using q7_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q7_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q7_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q7_acc_t>(extendedprice_raw) * static_cast<q7_acc_t>(100 - discount_raw);
}
#else
using q7_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q7_acc_t &value) {
	return value;
}

void AddRevenue(q7_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q7 revenue");
	}
}
#endif

enum class NationClass : uint8_t {
	Absent = 0,
	Nation1 = 1,
	Nation2 = 2,
};

struct Q7RuntimeConfig {
	bool dense_state = true;
	bool fixed_group_agg = true;
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

struct Q7OutputRow {
	std::string supp_nation;
	std::string cust_nation;
	int64_t year = 0;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q7OutputRow &lhs, const Q7OutputRow &rhs) {
	if (lhs.supp_nation != rhs.supp_nation) {
		return lhs.supp_nation < rhs.supp_nation;
	}
	if (lhs.cust_nation != rhs.cust_nation) {
		return lhs.cust_nation < rhs.cust_nation;
	}
	return lhs.year < rhs.year;
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

struct GroupSlotMeta {
	std::string supp_nation;
	std::string cust_nation;
	int64_t year = 0;
};

class Q7Kernel final : public KernelInstance {
public:
	explicit Q7Kernel(Q7Parameters params_p, Q7ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(),
	      customer_nations(kDefaultCustomerCapacity), supplier_nations(kDefaultSupplierCapacity),
	      order_cust_class(kDefaultOrderCapacity) {
		nation_class.assign(kMaxNationSlots, NationClass::Absent);
		if (config.dense_state) {
			customer_nations.WarmInitialize();
			supplier_nations.WarmInitialize();
			if (profile) {
				profile->used_dense_state = true;
			}
		}
		if (config.fixed_group_agg) {
			group_revenue.fill(0);
			group_has_revenue.fill(false);
			if (profile) {
				profile->used_fixed_group_agg = true;
			}
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q7InputId>(input_id)) {
		case Q7InputId::NATION:
			ConsumeNation(input);
			return;
		case Q7InputId::CUSTOMER:
			ConsumeCustomer(input);
			return;
		case Q7InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q7InputId::ORDERS:
			ConsumeOrders(input);
			return;
		case Q7InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		default:
			throw duckdb::InternalException("Q7 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		for (int slot = 0; slot < kMaxGroupSlots; slot++) {
			if (!group_has_revenue[static_cast<size_t>(slot)]) {
				continue;
			}
			const auto &meta = group_meta[static_cast<size_t>(slot)];
			Q7OutputRow row;
			row.supp_nation = meta.supp_nation;
			row.cust_nation = meta.cust_nation;
			row.year = meta.year;
			row.revenue = FromAccumulator(group_revenue[static_cast<size_t>(slot)]);
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
			output.SetValue(0, row, duckdb::Value(entry.supp_nation));
			output.SetValue(1, row, duckdb::Value(entry.cust_nation));
			output.SetValue(2, row, duckdb::Value::BIGINT(entry.year));
			output.SetValue(3, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void ConsumeNation(duckdb::DataChunk &input) {
		auto &nationkey_vector = input.data[Q7NationChunkLayout::NATIONKEY_INDEX];
		auto &name_vector = input.data[Q7NationChunkLayout::NAME_INDEX];
		if (IsFlatAllValid(nationkey_vector) && IsFlatAllValid(name_vector)) {
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots) {
					continue;
				}
				if (StringEquals(names[row], params.nation1)) {
					nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation1;
				} else if (StringEquals(names[row], params.nation2)) {
					nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation2;
				}
			}
			return;
		}
		duckdb::UnifiedVectorFormat nationkey_format;
		duckdb::UnifiedVectorFormat name_format;
		nationkey_vector.ToUnifiedFormat(input.size(), nationkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		auto nationkeys = duckdb::UnifiedVectorFormat::GetData<int32_t>(nationkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto name_value = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			if (StringEquals(name_value, params.nation1)) {
				nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation1;
			} else if (StringEquals(name_value, params.nation2)) {
				nation_class[static_cast<size_t>(nationkey)] = NationClass::Nation2;
			}
		}
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &custkey_vector = input.data[Q7CustomerChunkLayout::CUSTKEY_INDEX];
		auto &nationkey_vector = input.data[Q7CustomerChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots ||
				    nation_class[static_cast<size_t>(nationkey)] == NationClass::Absent) {
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
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    nation_class[static_cast<size_t>(nationkey)] == NationClass::Absent) {
				continue;
			}
			customer_nations.Store(custkeys[custkey_index], nationkey);
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &suppkey_vector = input.data[Q7SupplierChunkLayout::SUPPKEY_INDEX];
		auto &nationkey_vector = input.data[Q7SupplierChunkLayout::NATIONKEY_INDEX];
		if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(nationkey_vector)) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto nationkeys = duckdb::FlatVector::GetData<int32_t>(nationkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto nationkey = nationkeys[row];
				if (nationkey < 0 || nationkey >= kMaxNationSlots ||
				    nation_class[static_cast<size_t>(nationkey)] == NationClass::Absent) {
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
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto nationkey_index = nationkey_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!nationkey_format.validity.AllValid() && !nationkey_format.validity.RowIsValidUnsafe(nationkey_index))) {
				continue;
			}
			const auto nationkey = nationkeys[nationkey_index];
			if (nationkey < 0 || nationkey >= kMaxNationSlots ||
			    nation_class[static_cast<size_t>(nationkey)] == NationClass::Absent) {
				continue;
			}
			supplier_nations.Store(suppkeys[suppkey_index], nationkey);
		}
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		auto &orderkey_vector = input.data[Q7OrdersChunkLayout::ORDERKEY_INDEX];
		auto &custkey_vector = input.data[Q7OrdersChunkLayout::CUSTKEY_INDEX];
		if (IsFlatAllValid(orderkey_vector) && IsFlatAllValid(custkey_vector)) {
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto custkey = custkeys[row];
				const auto nationkey = customer_nations.Load(custkey);
				if (nationkey == kAbsentNation || nationkey < 0 || nationkey >= kMaxNationSlots) {
					continue;
				}
				const auto cust_class = nation_class[static_cast<size_t>(nationkey)];
				if (cust_class == NationClass::Absent) {
					continue;
				}
				order_cust_class.Store(orderkeys[row], static_cast<uint8_t>(cust_class));
			}
			return;
		}
		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat custkey_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto custkey_index = custkey_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index))) {
				continue;
			}
			const auto custkey = custkeys[custkey_index];
			const auto nationkey = customer_nations.Load(custkey);
			if (nationkey == kAbsentNation || nationkey < 0 || nationkey >= kMaxNationSlots) {
				continue;
			}
			const auto cust_class = nation_class[static_cast<size_t>(nationkey)];
			if (cust_class == NationClass::Absent) {
				continue;
			}
			order_cust_class.Store(orderkeys[orderkey_index], static_cast<uint8_t>(cust_class));
		}
	}

	int ComputeGroupSlot(NationClass supp_class, NationClass cust_class, int64_t year) const {
		if (year < kMinYear || year > kMaxYear) {
			return -1;
		}
		int direction = -1;
		if (supp_class == NationClass::Nation1 && cust_class == NationClass::Nation2) {
			direction = 0;
		} else if (supp_class == NationClass::Nation2 && cust_class == NationClass::Nation1) {
			direction = 1;
		} else {
			return -1;
		}
		return direction * 2 + static_cast<int>(year - kMinYear);
	}

	void EnsureGroupMeta(int slot, NationClass supp_class, NationClass cust_class, int64_t year) {
		if (!group_meta[static_cast<size_t>(slot)].supp_nation.empty()) {
			return;
		}
		GroupSlotMeta meta;
		meta.year = year;
		if (supp_class == NationClass::Nation1) {
			meta.supp_nation = params.nation1;
			meta.cust_nation = params.nation2;
		} else {
			meta.supp_nation = params.nation2;
			meta.cust_nation = params.nation1;
		}
		group_meta[static_cast<size_t>(slot)] = std::move(meta);
	}

	bool ProcessLineitemRow(int64_t orderkey, int64_t suppkey, duckdb::date_t shipdate, int64_t extendedprice_raw,
	                        int64_t discount_raw) {
		if (shipdate < params.shipdate_start || shipdate > params.shipdate_end) {
			return false;
		}
		const auto cust_class_raw = order_cust_class.Load(orderkey);
		if (cust_class_raw == kAbsentOrderCustClass) {
			return false;
		}
		const auto cust_class = static_cast<NationClass>(cust_class_raw);
		const auto supp_nation = supplier_nations.Load(suppkey);
		if (supp_nation == kAbsentNation || supp_nation < 0 || supp_nation >= kMaxNationSlots) {
			return false;
		}
		const auto supp_class = nation_class[static_cast<size_t>(supp_nation)];
		int64_t year = kMinYear;
		if (shipdate >= year_1996_start) {
			year = kMaxYear;
		}
		const int slot = ComputeGroupSlot(supp_class, cust_class, year);
		if (slot < 0) {
			return false;
		}
		EnsureGroupMeta(slot, supp_class, cust_class, year);
		if (profile) {
			profile->lineitem_rows_matched += 1;
		}
		AddRevenue(group_revenue[static_cast<size_t>(slot)], extendedprice_raw, discount_raw);
		group_has_revenue[static_cast<size_t>(slot)] = true;
		return true;
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		if (!has_qualifying_nations()) {
			return;
		}
		if (profile) {
			profile->lineitem_rows_scanned += input.size();
		}
		auto &orderkey_vector = input.data[Q7LineitemChunkLayout::ORDERKEY_INDEX];
		auto &suppkey_vector = input.data[Q7LineitemChunkLayout::SUPPKEY_INDEX];
		auto &shipdate_vector = input.data[Q7LineitemChunkLayout::SHIPDATE_INDEX];
		auto &extendedprice_vector = input.data[Q7LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q7LineitemChunkLayout::DISCOUNT_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(orderkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(shipdate_vector) && IsFlatAllValid(extendedprice_vector) &&
		    IsFlatAllValid(discount_vector)) {
			if (profile) {
				profile->lineitem_flat_fast_rows += input.size();
			}
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(orderkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(shipdate_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				(void)ProcessLineitemRow(orderkeys[row], suppkeys[row], shipdates[row], extendedprices[row],
				                         discounts[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(orderkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(shipdate_vector) && IsDictionaryOverFlat(extendedprice_vector) &&
		    IsDictionaryOverFlat(discount_vector)) {
			if (profile) {
				profile->lineitem_dict_fast_rows += input.size();
			}
			auto &orderkey_sel = duckdb::DictionaryVector::SelVector(orderkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &shipdate_sel = duckdb::DictionaryVector::SelVector(shipdate_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto orderkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(orderkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto shipdates = duckdb::FlatVector::GetData<duckdb::date_t>(duckdb::DictionaryVector::Child(shipdate_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto &orderkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(orderkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &shipdate_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(shipdate_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto orderkey_index = orderkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto shipdate_index = shipdate_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				if (!orderkey_validity.RowIsValid(orderkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
				    !shipdate_validity.RowIsValid(shipdate_index) ||
				    !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				(void)ProcessLineitemRow(orderkeys[orderkey_index], suppkeys[suppkey_index], shipdates[shipdate_index],
				                         extendedprices[extendedprice_index], discounts[discount_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat orderkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat shipdate_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		orderkey_vector.ToUnifiedFormat(input.size(), orderkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		shipdate_vector.ToUnifiedFormat(input.size(), shipdate_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto orderkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(orderkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto shipdates = duckdb::UnifiedVectorFormat::GetData<duckdb::date_t>(shipdate_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto orderkey_index = orderkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto shipdate_index = shipdate_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!orderkey_format.validity.AllValid() && !orderkey_format.validity.RowIsValidUnsafe(orderkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!shipdate_format.validity.AllValid() && !shipdate_format.validity.RowIsValidUnsafe(shipdate_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			(void)ProcessLineitemRow(orderkeys[orderkey_index], suppkeys[suppkey_index], shipdates[shipdate_index],
			                         extendedprices[extendedprice_index], discounts[discount_index]);
		}
	}

	bool has_qualifying_nations() const {
		for (const auto cls : nation_class) {
			if (cls != NationClass::Absent) {
				return true;
			}
		}
		return false;
	}

	Q7Parameters params;
	Q7ProfileCounters *profile;
	Q7RuntimeConfig config;
	std::vector<NationClass> nation_class;
	DenseKeyStore<int32_t, kAbsentNation> customer_nations;
	DenseKeyStore<int32_t, kAbsentNation> supplier_nations;
	DenseKeyStore<uint8_t, kAbsentOrderCustClass> order_cust_class;
	const duckdb::date_t year_1996_start = duckdb::Date::FromString("1996-01-01");
	std::array<q7_acc_t, kMaxGroupSlots> group_revenue {};
	std::array<bool, kMaxGroupSlots> group_has_revenue {};
	std::array<GroupSlotMeta, kMaxGroupSlots> group_meta {};
	std::vector<Q7OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ7Kernel(const Q7Parameters &params) {
	return duckdb::make_uniq<Q7Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ7ProfilingKernel(const Q7Parameters &params, Q7ProfileCounters *profile) {
	return duckdb::make_uniq<Q7Kernel>(params, profile);
}

} // namespace accelerator_codegen
