#include "tpch/q22/duckdb/base/query.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultCustomerCapacity = 150001;
constexpr size_t kCountrySlotCount = 100;

#if defined(__SIZEOF_INT128__)
using q22_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q22_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddBalance(q22_acc_t &accumulator, int64_t acctbal_raw) {
	accumulator += static_cast<q22_acc_t>(acctbal_raw);
}
#else
using q22_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q22_acc_t &value) {
	return value;
}

void AddBalance(q22_acc_t &accumulator, int64_t acctbal_raw) {
	duckdb::hugeint_t balance_value;
	if (!duckdb::Hugeint::TryConvert(acctbal_raw, balance_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, balance_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q22 balance");
	}
}
#endif

struct Q22RuntimeConfig {
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

struct GroupState {
	int64_t customer_count = 0;
	q22_acc_t total_balance = 0;
};

struct Q22OutputRow {
	std::string country_code;
	int64_t customer_count = 0;
	duckdb::hugeint_t total_balance;
};

bool BetterOutputRow(const Q22OutputRow &lhs, const Q22OutputRow &rhs) {
	return lhs.country_code < rhs.country_code;
}

bool TryExtractCountryCodeInt(const duckdb::string_t &phone, uint8_t &code) {
	if (phone.GetSize() < 2) {
		return false;
	}
	const auto *data = phone.GetData();
	if (data[0] < '0' || data[0] > '9' || data[1] < '0' || data[1] > '9') {
		return false;
	}
	code = static_cast<uint8_t>((data[0] - '0') * 10 + (data[1] - '0'));
	return true;
}

class Q22Kernel final : public KernelInstance {
public:
	explicit Q22Kernel(Q22Parameters params_p, Q22ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config() {
		for (const auto &code : params.country_codes) {
			const int value = std::stoi(code);
			if (value >= 0 && value < static_cast<int>(kCountrySlotCount)) {
				allowed_code_mask[static_cast<size_t>(value)] = 1;
				country_labels[static_cast<size_t>(value)] = code;
			}
		}
		active_customers.reserve(160000);
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q22InputId>(input_id)) {
		case Q22InputId::ORDERS:
			ConsumeOrders(input);
			return;
		case Q22InputId::CUSTOMER:
			ConsumeCustomer(input);
			return;
		default:
			throw duckdb::InternalException("Q22 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		std::array<GroupState, kCountrySlotCount> groups {};
		for (const auto custkey : active_customers) {
			if (custkey < 0 || static_cast<size_t>(custkey) >= has_orders.size()) {
				continue;
			}
			if (has_orders[static_cast<size_t>(custkey)] != 0) {
				continue;
			}
			const auto acctbal = customer_acctbal[static_cast<size_t>(custkey)];
			if (positive_balance_count > 0) {
#if defined(__SIZEOF_INT128__)
				const auto lhs = static_cast<q22_acc_t>(acctbal) * static_cast<q22_acc_t>(positive_balance_count);
				if (lhs <= positive_balance_sum) {
					continue;
				}
#else
				duckdb::hugeint_t lhs;
				duckdb::hugeint_t count_value;
				duckdb::hugeint_t product_value;
				if (!duckdb::Hugeint::TryConvert(acctbal, lhs) ||
				    !duckdb::Hugeint::TryConvert(positive_balance_count, count_value) ||
				    !duckdb::Hugeint::TryMultiply(lhs, count_value, product_value) ||
				    !duckdb::Hugeint::GreaterThan(product_value, positive_balance_sum)) {
					continue;
				}
#endif
			} else {
				continue;
			}
			const auto code = customer_country_code[static_cast<size_t>(custkey)];
			auto &group = groups[code];
			group.customer_count += 1;
			AddBalance(group.total_balance, acctbal);
		}

		output_rows.reserve(8);
		for (size_t code = 0; code < kCountrySlotCount; code++) {
			if (groups[code].customer_count == 0) {
				continue;
			}
			Q22OutputRow row;
			row.country_code = country_labels[code];
			row.customer_count = groups[code].customer_count;
			row.total_balance = FromAccumulator(groups[code].total_balance);
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
			output.SetValue(0, row, duckdb::Value(entry.country_code));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.customer_count));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.total_balance, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	void EnsureCustomerCapacity(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(custkey) + 1;
		if (required <= has_orders.size()) {
			return;
		}
		size_t target = required;
		if (!has_orders.empty()) {
			const size_t doubled = has_orders.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultCustomerCapacity));
		}
		has_orders.resize(target, 0);
		customer_valid.resize(target, 0);
		customer_country_code.resize(target, 0);
		customer_acctbal.resize(target, 0);
	}

	void MarkCustomerHasOrders(int64_t custkey) {
		if (custkey < 0) {
			return;
		}
		EnsureCustomerCapacity(custkey);
		has_orders[static_cast<size_t>(custkey)] = 1;
	}

	void ConsumeMatchingCustomer(int64_t custkey, uint8_t country_code, int64_t acctbal_raw) {
		if (custkey < 0) {
			return;
		}
		EnsureCustomerCapacity(custkey);
		const auto index = static_cast<size_t>(custkey);
		if (customer_valid[index] == 0) {
			customer_valid[index] = 1;
			active_customers.push_back(custkey);
		}
		customer_country_code[index] = country_code;
		customer_acctbal[index] = acctbal_raw;
		if (acctbal_raw > 0) {
			positive_balance_sum += acctbal_raw;
			positive_balance_count += 1;
		}
		if (profile) {
			profile->customer_rows_matched += 1;
		}
	}

	bool CountryAllowed(const duckdb::string_t &phone, uint8_t &country_code) const {
		if (!TryExtractCountryCodeInt(phone, country_code)) {
			return false;
		}
		return allowed_code_mask[country_code] != 0;
	}

	void ConsumeOrders(duckdb::DataChunk &input) {
		auto &custkey_vector = input.data[Q22OrdersChunkLayout::CUSTKEY_INDEX];
		if (config.orders_flat_fast && IsFlatAllValid(custkey_vector)) {
			if (profile) {
				profile->orders_flat_fast_rows += input.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				MarkCustomerHasOrders(custkeys[row]);
			}
			return;
		}

		if (config.orders_dict_fast && IsDictionaryOverFlat(custkey_vector)) {
			if (profile) {
				profile->orders_dict_fast_rows += input.size();
			}
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->orders_rows_scanned += 1;
				}
				const auto custkey_index = custkey_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				MarkCustomerHasOrders(custkeys[custkey_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->orders_rows_scanned += 1;
			}
			const auto custkey_index = custkey_format.sel->get_index(row);
			if (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			MarkCustomerHasOrders(custkeys[custkey_index]);
		}
	}

	void ConsumeCustomer(duckdb::DataChunk &input) {
		auto &custkey_vector = input.data[Q22CustomerChunkLayout::CUSTKEY_INDEX];
		auto &phone_vector = input.data[Q22CustomerChunkLayout::PHONE_INDEX];
		auto &acctbal_vector = input.data[Q22CustomerChunkLayout::ACCTBAL_INDEX];

		if (config.customer_flat_fast && IsFlatAllValid(custkey_vector) && IsFlatAllValid(phone_vector) &&
		    IsFlatAllValid(acctbal_vector)) {
			if (profile) {
				profile->customer_flat_fast_rows += input.size();
			}
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
			auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
			auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->customer_rows_scanned += 1;
				}
				uint8_t country_code = 0;
				if (!CountryAllowed(phones[row], country_code)) {
					continue;
				}
				ConsumeMatchingCustomer(custkeys[row], country_code, acctbals[row]);
			}
			return;
		}

		if (config.customer_dict_fast && IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(phone_vector) &&
		    IsDictionaryOverFlat(acctbal_vector)) {
			if (profile) {
				profile->customer_dict_fast_rows += input.size();
			}
			auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
			auto &phone_sel = duckdb::DictionaryVector::SelVector(phone_vector);
			auto &acctbal_sel = duckdb::DictionaryVector::SelVector(acctbal_vector);
			auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
			auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(phone_vector));
			auto acctbals = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(acctbal_vector));
			auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
			auto &phone_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(phone_vector));
			auto &acctbal_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(acctbal_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->customer_rows_scanned += 1;
				}
				const auto custkey_index = custkey_sel.get_index(row);
				const auto phone_index = phone_sel.get_index(row);
				const auto acctbal_index = acctbal_sel.get_index(row);
				if (!custkey_validity.RowIsValid(custkey_index) || !phone_validity.RowIsValid(phone_index) ||
				    !acctbal_validity.RowIsValid(acctbal_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				uint8_t country_code = 0;
				if (!CountryAllowed(phones[phone_index], country_code)) {
					continue;
				}
				ConsumeMatchingCustomer(custkeys[custkey_index], country_code, acctbals[acctbal_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat custkey_format;
		duckdb::UnifiedVectorFormat phone_format;
		duckdb::UnifiedVectorFormat acctbal_format;
		custkey_vector.ToUnifiedFormat(input.size(), custkey_format);
		phone_vector.ToUnifiedFormat(input.size(), phone_format);
		acctbal_vector.ToUnifiedFormat(input.size(), acctbal_format);
		auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
		auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->customer_rows_scanned += 1;
			}
			const auto custkey_index = custkey_format.sel->get_index(row);
			const auto phone_index = phone_format.sel->get_index(row);
			const auto acctbal_index = acctbal_format.sel->get_index(row);
			if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
			    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
			    (!acctbal_format.validity.AllValid() && !acctbal_format.validity.RowIsValidUnsafe(acctbal_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto phone = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index];
			uint8_t country_code = 0;
			if (!CountryAllowed(phone, country_code)) {
				continue;
			}
			ConsumeMatchingCustomer(custkeys[custkey_index], country_code, acctbals[acctbal_index]);
		}
	}

	Q22Parameters params;
	Q22ProfileCounters *profile;
	Q22RuntimeConfig config;
	std::array<uint8_t, kCountrySlotCount> allowed_code_mask {};
	std::array<std::string, kCountrySlotCount> country_labels {};
	std::vector<uint8_t> has_orders;
	std::vector<uint8_t> customer_valid;
	std::vector<uint8_t> customer_country_code;
	std::vector<int64_t> customer_acctbal;
	std::vector<int64_t> active_customers;
#if defined(__SIZEOF_INT128__)
	__int128_t positive_balance_sum = 0;
#else
	duckdb::hugeint_t positive_balance_sum;
#endif
	int64_t positive_balance_count = 0;
	std::vector<Q22OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ22Kernel(const Q22Parameters &params) {
	return duckdb::make_uniq<Q22Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ22ProfilingKernel(const Q22Parameters &params, Q22ProfileCounters *profile) {
	return duckdb::make_uniq<Q22Kernel>(params, profile);
}

} // namespace accelerator_codegen
