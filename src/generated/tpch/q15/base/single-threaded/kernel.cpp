#include "tpch/q15/base/query.hpp"

#include <algorithm>
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
constexpr int64_t kDefaultSupplierCapacity = 10001;

#if defined(__SIZEOF_INT128__)
using q15_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q15_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddRevenue(q15_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	accumulator += static_cast<q15_acc_t>(extendedprice_raw) * static_cast<q15_acc_t>(100 - discount_raw);
}
#else
using q15_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q15_acc_t &value) {
	return value;
}

void AddRevenue(q15_acc_t &accumulator, int64_t extendedprice_raw, int64_t discount_raw) {
	duckdb::hugeint_t price_value;
	duckdb::hugeint_t factor_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(extendedprice_raw, price_value) ||
	    !duckdb::Hugeint::TryConvert(static_cast<int64_t>(100 - discount_raw), factor_value) ||
	    !duckdb::Hugeint::TryMultiply(price_value, factor_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q15 revenue");
	}
}
#endif

struct Q15RuntimeConfig {
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

struct SupplierRow {
	std::string name;
	std::string address;
	std::string phone;
};

struct Q15OutputRow {
	int64_t suppkey = 0;
	std::string name;
	std::string address;
	std::string phone;
	duckdb::hugeint_t revenue;
};

bool BetterOutputRow(const Q15OutputRow &lhs, const Q15OutputRow &rhs) {
	return lhs.suppkey < rhs.suppkey;
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class Q15Kernel final : public KernelInstance {
public:
	explicit Q15Kernel(Q15Parameters params_p) : params(std::move(params_p)), config() {
		revenue_by_supplier.assign(static_cast<size_t>(kDefaultSupplierCapacity), 0);
		has_revenue_by_supplier.assign(static_cast<size_t>(kDefaultSupplierCapacity), false);
		supplier_capacity = kDefaultSupplierCapacity;
		active_suppkeys.reserve(4096);
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q15InputId>(input_id)) {
		case Q15InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		case Q15InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		default:
			throw duckdb::InternalException("Q15 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		duckdb::hugeint_t max_revenue;
		bool has_max_revenue = false;
		for (const auto suppkey : active_suppkeys) {
			if (suppkey < 0 || static_cast<uint64_t>(suppkey) >= static_cast<uint64_t>(supplier_capacity)) {
				continue;
			}
			const auto revenue = FromAccumulator(revenue_by_supplier[static_cast<size_t>(suppkey)]);
			if (!has_max_revenue || duckdb::Hugeint::GreaterThan(revenue, max_revenue)) {
				max_revenue = revenue;
				has_max_revenue = true;
			}
		}

		output_rows.clear();
		if (!has_max_revenue) {
			output_offset = 0;
			finalized = true;
			return;
		}

		output_rows.reserve(active_suppkeys.size());
		for (const auto suppkey : active_suppkeys) {
			if (suppkey < 0 || static_cast<uint64_t>(suppkey) >= static_cast<uint64_t>(supplier_capacity)) {
				continue;
			}
			const auto revenue = FromAccumulator(revenue_by_supplier[static_cast<size_t>(suppkey)]);
			if (!duckdb::Hugeint::Equals(revenue, max_revenue)) {
				continue;
			}
			const auto supplier_it = suppliers.find(suppkey);
			if (supplier_it == suppliers.end()) {
				continue;
			}
			Q15OutputRow row;
			row.suppkey = suppkey;
			row.name = supplier_it->second.name;
			row.address = supplier_it->second.address;
			row.phone = supplier_it->second.phone;
			row.revenue = revenue;
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.suppkey));
			output.SetValue(1, row, duckdb::Value(entry.name));
			output.SetValue(2, row, duckdb::Value(entry.address));
			output.SetValue(3, row, duckdb::Value(entry.phone));
			output.SetValue(4, row, duckdb::Value::DECIMAL(entry.revenue, kRevenueDecimalWidth, kRevenueDecimalScale));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void EnsureSupplierCapacity(int64_t suppkey) {
		if (suppkey < 0) {
			return;
		}
		const auto required = suppkey + 1;
		if (required <= supplier_capacity) {
			return;
		}
		int64_t target = required;
		if (supplier_capacity > 0) {
			const int64_t doubled = supplier_capacity > (std::numeric_limits<int64_t>::max() / 2)
			                              ? std::numeric_limits<int64_t>::max()
			                              : supplier_capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		}
		revenue_by_supplier.resize(static_cast<size_t>(target), 0);
		has_revenue_by_supplier.resize(static_cast<size_t>(target), false);
		supplier_capacity = target;
	}

	void AccumulateRevenue(int64_t suppkey, int64_t extendedprice_raw, int64_t discount_raw) {
		if (suppkey < 0) {
			return;
		}
		EnsureSupplierCapacity(suppkey);
		const auto index = static_cast<size_t>(suppkey);
		if (!has_revenue_by_supplier[index]) {
			has_revenue_by_supplier[index] = true;
			active_suppkeys.push_back(suppkey);
		}
		AddRevenue(revenue_by_supplier[index], extendedprice_raw, discount_raw);
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		auto &suppkey_vector = input.data[Q15LineitemChunkLayout::SUPPKEY_INDEX];
		auto &extendedprice_vector = input.data[Q15LineitemChunkLayout::EXTENDEDPRICE_INDEX];
		auto &discount_vector = input.data[Q15LineitemChunkLayout::DISCOUNT_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(extendedprice_vector) &&
		    IsFlatAllValid(discount_vector)) {
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto extendedprices = duckdb::FlatVector::GetData<int64_t>(extendedprice_vector);
			auto discounts = duckdb::FlatVector::GetData<int64_t>(discount_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				AccumulateRevenue(suppkeys[row], extendedprices[row], discounts[row]);
			}
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(extendedprice_vector) && IsDictionaryOverFlat(discount_vector)) {
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &extendedprice_sel = duckdb::DictionaryVector::SelVector(extendedprice_vector);
			auto &discount_sel = duckdb::DictionaryVector::SelVector(discount_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto extendedprices =
			    duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto discounts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(discount_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &extendedprice_validity =
			    duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(extendedprice_vector));
			auto &discount_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(discount_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto extendedprice_index = extendedprice_sel.get_index(row);
				const auto discount_index = discount_sel.get_index(row);
				if (!suppkey_validity.RowIsValid(suppkey_index) || !extendedprice_validity.RowIsValid(extendedprice_index) ||
				    !discount_validity.RowIsValid(discount_index)) {
					continue;
				}
				AccumulateRevenue(suppkeys[suppkey_index], extendedprices[extendedprice_index],
				                  discounts[discount_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat extendedprice_format;
		duckdb::UnifiedVectorFormat discount_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		extendedprice_vector.ToUnifiedFormat(input.size(), extendedprice_format);
		discount_vector.ToUnifiedFormat(input.size(), discount_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto extendedprices = duckdb::UnifiedVectorFormat::GetData<int64_t>(extendedprice_format);
		auto discounts = duckdb::UnifiedVectorFormat::GetData<int64_t>(discount_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto extendedprice_index = extendedprice_format.sel->get_index(row);
			const auto discount_index = discount_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!extendedprice_format.validity.AllValid() &&
			     !extendedprice_format.validity.RowIsValidUnsafe(extendedprice_index)) ||
			    (!discount_format.validity.AllValid() && !discount_format.validity.RowIsValidUnsafe(discount_index))) {
				continue;
			}
			AccumulateRevenue(suppkeys[suppkey_index], extendedprices[extendedprice_index], discounts[discount_index]);
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		duckdb::UnifiedVectorFormat phone_format;
		input.data[Q15SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(), suppkey_format);
		input.data[Q15SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
		input.data[Q15SupplierChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(input.size(), address_format);
		input.data[Q15SupplierChunkLayout::PHONE_INDEX].ToUnifiedFormat(input.size(), phone_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (suppkey < 0 || static_cast<uint64_t>(suppkey) >= static_cast<uint64_t>(supplier_capacity) ||
			    !has_revenue_by_supplier[static_cast<size_t>(suppkey)]) {
				continue;
			}
			SupplierRow supplier;
			supplier.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			supplier.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			supplier.phone = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index]);
			suppliers.emplace(suppkey, std::move(supplier));
		}
	}

	Q15Parameters params;
	Q15RuntimeConfig config;
	int64_t supplier_capacity = 0;
	std::vector<q15_acc_t> revenue_by_supplier;
	std::vector<bool> has_revenue_by_supplier;
	std::vector<int64_t> active_suppkeys;
	std::unordered_map<int64_t, SupplierRow> suppliers;
	std::vector<Q15OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ15Kernel(const Q15Parameters &params) {
	return duckdb::make_uniq<Q15Kernel>(params);
}

} // namespace accelerator_codegen
