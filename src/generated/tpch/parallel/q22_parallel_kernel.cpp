#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/parallel/parallel_kernel.hpp"
#include "accelerator_codegen/parallel/parallel_runtime.hpp"
#include "accelerator_codegen/tpch_q22.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
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
#else
using q22_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q22_acc_t &value) {
	return value;
}
#endif

struct Q22OutputRow {
	std::string country_code;
	int64_t customer_count = 0;
	duckdb::hugeint_t total_balance;
};

struct PrefixSetup {
	std::array<uint8_t, kCountrySlotCount> allowed_code_mask {};
	std::array<std::string, kCountrySlotCount> country_labels {};
};

struct AveragePartial {
	q22_acc_t positive_balance_sum = 0;
	int64_t positive_balance_count = 0;
	size_t rows_scanned = 0;
	size_t prefix_matched = 0;
	size_t positive_matched = 0;
};

struct PrefixGroupPartial {
	std::array<q22_acc_t, kCountrySlotCount> totals {};
	std::array<int64_t, kCountrySlotCount> counts {};
};

struct PassProfile {
	size_t rows_scanned = 0;
	size_t prefix_matched = 0;
	size_t positive_matched = 0;
	size_t above_average = 0;
	size_t anti_join_survivors = 0;
	size_t flat_rows = 0;
	size_t dict_rows = 0;
	size_t fallback_rows = 0;
	int64_t process_ns = 0;
};

struct Q22ParallelProfile {
	int64_t prefix_setup_ns = 0;
	int64_t orders_scan_ns = 0;
	int64_t orders_merge_ns = 0;
	int64_t customer_pass1_ns = 0;
	int64_t average_reduce_ns = 0;
	int64_t customer_pass2_ns = 0;
	int64_t prefix_merge_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t orders_rows_scanned = 0;
	size_t orders_flat_rows = 0;
	size_t orders_fallback_rows = 0;
	size_t customer_pass1_rows = 0;
	size_t customer_pass1_prefix = 0;
	size_t customer_pass1_positive = 0;
	size_t customer_pass2_rows = 0;
	size_t customer_pass2_prefix = 0;
	size_t customer_pass2_above_average = 0;
	size_t customer_pass2_survivors = 0;
	size_t customer_pass1_flat_rows = 0;
	size_t customer_pass2_flat_rows = 0;
	size_t customer_pass1_fallback_rows = 0;
	size_t customer_pass2_fallback_rows = 0;
	int64_t positive_balance_count = 0;
	size_t orders_bitmap_words = 0;
	size_t orders_bitmap_bytes = 0;
	size_t orders_worker_bitmap_bytes = 0;
	size_t min_worker_pass1_ns = 0;
	size_t max_worker_pass1_ns = 0;
	size_t median_worker_pass1_ns = 0;
	size_t min_worker_pass2_ns = 0;
	size_t max_worker_pass2_ns = 0;
	size_t median_worker_pass2_ns = 0;
};

bool BetterOutputRow(const Q22OutputRow &lhs, const Q22OutputRow &rhs) {
	return lhs.country_code < rhs.country_code;
}

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

bool CountryAllowed(uint8_t code, const PrefixSetup &setup) {
	return code < kCountrySlotCount && setup.allowed_code_mask[code] != 0;
}

bool CountryAllowed(const duckdb::string_t &phone, const PrefixSetup &setup, uint8_t &code) {
	return TryExtractCountryCodeInt(phone, code) && CountryAllowed(code, setup);
}

void AddPositiveBalance(AveragePartial &partial, int64_t acctbal_raw) {
#if defined(__SIZEOF_INT128__)
	partial.positive_balance_sum += static_cast<q22_acc_t>(acctbal_raw);
#else
	duckdb::hugeint_t addend;
	if (!duckdb::Hugeint::TryConvert(acctbal_raw, addend) ||
	    (partial.positive_balance_count > 0 && !duckdb::Hugeint::TryAddInPlace(partial.positive_balance_sum, addend))) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q22 positive balance sum");
	}
	if (partial.positive_balance_count == 0) {
		partial.positive_balance_sum = addend;
	}
#endif
	partial.positive_balance_count += 1;
}

void AddGroupBalance(PrefixGroupPartial &groups, uint8_t code, int64_t acctbal_raw) {
	groups.counts[code] += 1;
#if defined(__SIZEOF_INT128__)
	groups.totals[code] += static_cast<q22_acc_t>(acctbal_raw);
#else
	duckdb::hugeint_t addend;
	if (!duckdb::Hugeint::TryConvert(acctbal_raw, addend) ||
	    (groups.counts[code] > 1 && !duckdb::Hugeint::TryAddInPlace(groups.totals[code], addend))) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q22 balance");
	}
	if (groups.counts[code] == 1) {
		groups.totals[code] = addend;
	}
#endif
}

bool BalanceGreaterThanAverage(int64_t acctbal_raw, q22_acc_t positive_balance_sum, int64_t positive_balance_count) {
	if (positive_balance_count <= 0) {
		return false;
	}
#if defined(__SIZEOF_INT128__)
	const auto lhs = static_cast<q22_acc_t>(acctbal_raw) * static_cast<q22_acc_t>(positive_balance_count);
	return lhs > positive_balance_sum;
#else
	duckdb::hugeint_t lhs;
	duckdb::hugeint_t count_value;
	duckdb::hugeint_t product_value;
	return duckdb::Hugeint::TryConvert(acctbal_raw, lhs) && duckdb::Hugeint::TryConvert(positive_balance_count, count_value) &&
	       duckdb::Hugeint::TryMultiply(lhs, count_value, product_value) &&
	       duckdb::Hugeint::GreaterThan(product_value, positive_balance_sum);
#endif
}

void MergeAveragePartial(AveragePartial &global, const AveragePartial &local) {
#if defined(__SIZEOF_INT128__)
	global.positive_balance_sum += local.positive_balance_sum;
#else
	if (local.positive_balance_count > 0) {
		if (global.positive_balance_count == 0) {
			global.positive_balance_sum = local.positive_balance_sum;
		} else if (!duckdb::Hugeint::TryAddInPlace(global.positive_balance_sum, local.positive_balance_sum)) {
			throw duckdb::OutOfRangeException("Overflow while merging Q22 positive balance sum");
		}
	}
#endif
	global.positive_balance_count += local.positive_balance_count;
	global.rows_scanned += local.rows_scanned;
	global.prefix_matched += local.prefix_matched;
	global.positive_matched += local.positive_matched;
}

void MergePrefixGroups(PrefixGroupPartial &global, const PrefixGroupPartial &local) {
	for (size_t code = 0; code < kCountrySlotCount; code++) {
		if (local.counts[code] == 0) {
			continue;
		}
#if defined(__SIZEOF_INT128__)
		global.totals[code] += local.totals[code];
#else
		if (global.counts[code] == 0) {
			global.totals[code] = local.totals[code];
		} else if (!duckdb::Hugeint::TryAddInPlace(global.totals[code], local.totals[code])) {
			throw duckdb::OutOfRangeException("Overflow while merging Q22 prefix totals");
		}
#endif
		global.counts[code] += local.counts[code];
	}
}

class CustomerHasOrdersBitmap {
public:
	CustomerHasOrdersBitmap() : CustomerHasOrdersBitmap((static_cast<size_t>(kDefaultCustomerCapacity) >> 6) + 1) {
	}

	explicit CustomerHasOrdersBitmap(size_t initial_words) {
		words.assign(initial_words, 0);
	}

	void Mark(int64_t custkey) {
		if (custkey < 0) {
			overflow.insert(custkey);
			return;
		}
		const auto key = static_cast<uint64_t>(custkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			if (custkey > 1'000'000'000) {
				overflow.insert(custkey);
				return;
			}
			words.resize(static_cast<size_t>(word_index) + 1, 0);
		}
		words[static_cast<size_t>(word_index)] |= (uint64_t {1} << (key & 63));
	}

	bool Has(int64_t custkey) const {
		if (custkey < 0) {
			return overflow.find(custkey) != overflow.end();
		}
		const auto key = static_cast<uint64_t>(custkey);
		const auto word_index = key >> 6;
		if (word_index >= words.size()) {
			return false;
		}
		return (words[static_cast<size_t>(word_index)] >> (key & 63)) & 1;
	}

	void MergeFrom(const CustomerHasOrdersBitmap &other) {
		if (other.words.size() > words.size()) {
			words.resize(other.words.size(), 0);
		}
		for (size_t index = 0; index < other.words.size(); index++) {
			words[index] |= other.words[index];
		}
		overflow.insert(other.overflow.begin(), other.overflow.end());
	}

	size_t WordCount() const {
		return words.size();
	}

	size_t LogicalBytes() const {
		return words.size() * sizeof(uint64_t);
	}

private:
	std::vector<uint64_t> words;
	std::unordered_set<int64_t> overflow;
};

PrefixSetup BuildPrefixSetup(const Q22Parameters &params) {
	PrefixSetup setup;
	for (const auto &code : params.country_codes) {
		const int value = std::stoi(code);
		if (value >= 0 && value < static_cast<int>(kCountrySlotCount)) {
			setup.allowed_code_mask[static_cast<size_t>(value)] = 1;
			setup.country_labels[static_cast<size_t>(value)] = code;
		}
	}
	return setup;
}

void AccumulateOrdersChunk(duckdb::DataChunk &chunk, CustomerHasOrdersBitmap &bitmap, PassProfile &profile) {
	auto &custkey_vector = chunk.data[Q22OrdersChunkLayout::CUSTKEY_INDEX];
	if (IsFlatAllValid(custkey_vector)) {
		profile.flat_rows += chunk.size();
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			bitmap.Mark(custkeys[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(custkey_vector)) {
		profile.dict_rows += chunk.size();
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			const auto custkey_index = custkey_sel.get_index(row);
			if (!custkey_validity.RowIsValid(custkey_index)) {
				profile.fallback_rows++;
				continue;
			}
			bitmap.Mark(custkeys[custkey_index]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat custkey_format;
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		profile.rows_scanned++;
		const auto custkey_index = custkey_format.sel->get_index(row);
		if (!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) {
			continue;
		}
		bitmap.Mark(custkeys[custkey_index]);
	}
}

void AccumulateAverageChunk(duckdb::DataChunk &chunk, const PrefixSetup &setup, AveragePartial &partial, PassProfile &profile) {
	auto &phone_vector = chunk.data[Q22CustomerChunkLayout::PHONE_INDEX];
	auto &acctbal_vector = chunk.data[Q22CustomerChunkLayout::ACCTBAL_INDEX];

	const auto process_row = [&](uint8_t country_code, int64_t acctbal_raw) {
		profile.prefix_matched++;
		partial.prefix_matched++;
		if (acctbal_raw <= 0) {
			return;
		}
		profile.positive_matched++;
		partial.positive_matched++;
		AddPositiveBalance(partial, acctbal_raw);
	};

	if (IsFlatAllValid(phone_vector) && IsFlatAllValid(acctbal_vector)) {
		profile.flat_rows += chunk.size();
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			partial.rows_scanned++;
			uint8_t country_code = 0;
			if (!CountryAllowed(phones[row], setup, country_code)) {
				continue;
			}
			process_row(country_code, acctbals[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(phone_vector) && IsDictionaryOverFlat(acctbal_vector)) {
		profile.dict_rows += chunk.size();
		auto &phone_sel = duckdb::DictionaryVector::SelVector(phone_vector);
		auto &acctbal_sel = duckdb::DictionaryVector::SelVector(acctbal_vector);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(phone_vector));
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(acctbal_vector));
		auto &phone_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(phone_vector));
		auto &acctbal_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(acctbal_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			partial.rows_scanned++;
			const auto phone_index = phone_sel.get_index(row);
			const auto acctbal_index = acctbal_sel.get_index(row);
			if (!phone_validity.RowIsValid(phone_index) || !acctbal_validity.RowIsValid(acctbal_index)) {
				profile.fallback_rows++;
				continue;
			}
			uint8_t country_code = 0;
			if (!CountryAllowed(phones[phone_index], setup, country_code)) {
				continue;
			}
			process_row(country_code, acctbals[acctbal_index]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat phone_format;
	duckdb::UnifiedVectorFormat acctbal_format;
	phone_vector.ToUnifiedFormat(chunk.size(), phone_format);
	acctbal_vector.ToUnifiedFormat(chunk.size(), acctbal_format);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		profile.rows_scanned++;
		partial.rows_scanned++;
		const auto phone_index = phone_format.sel->get_index(row);
		const auto acctbal_index = acctbal_format.sel->get_index(row);
		if ((!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
		    (!acctbal_format.validity.AllValid() && !acctbal_format.validity.RowIsValidUnsafe(acctbal_index))) {
			continue;
		}
		const auto phone = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index];
		uint8_t country_code = 0;
		if (!CountryAllowed(phone, setup, country_code)) {
			continue;
		}
		process_row(country_code, acctbals[acctbal_index]);
	}
}

void AccumulateQualifyingChunk(duckdb::DataChunk &chunk, const PrefixSetup &setup, const CustomerHasOrdersBitmap &has_orders,
                               q22_acc_t positive_balance_sum, int64_t positive_balance_count, PrefixGroupPartial &groups,
                               PassProfile &profile) {
	auto &custkey_vector = chunk.data[Q22CustomerChunkLayout::CUSTKEY_INDEX];
	auto &phone_vector = chunk.data[Q22CustomerChunkLayout::PHONE_INDEX];
	auto &acctbal_vector = chunk.data[Q22CustomerChunkLayout::ACCTBAL_INDEX];

	const auto process_row = [&](int64_t custkey, uint8_t country_code, int64_t acctbal_raw) {
		profile.prefix_matched++;
		if (!BalanceGreaterThanAverage(acctbal_raw, positive_balance_sum, positive_balance_count)) {
			return;
		}
		profile.above_average++;
		if (has_orders.Has(custkey)) {
			return;
		}
		profile.anti_join_survivors++;
		AddGroupBalance(groups, country_code, acctbal_raw);
	};

	if (IsFlatAllValid(custkey_vector) && IsFlatAllValid(phone_vector) && IsFlatAllValid(acctbal_vector)) {
		profile.flat_rows += chunk.size();
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(custkey_vector);
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(phone_vector);
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(acctbal_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			uint8_t country_code = 0;
			if (!CountryAllowed(phones[row], setup, country_code)) {
				continue;
			}
			process_row(custkeys[row], country_code, acctbals[row]);
		}
		return;
	}

	if (IsDictionaryOverFlat(custkey_vector) && IsDictionaryOverFlat(phone_vector) && IsDictionaryOverFlat(acctbal_vector)) {
		profile.dict_rows += chunk.size();
		auto &custkey_sel = duckdb::DictionaryVector::SelVector(custkey_vector);
		auto &phone_sel = duckdb::DictionaryVector::SelVector(phone_vector);
		auto &acctbal_sel = duckdb::DictionaryVector::SelVector(acctbal_vector);
		auto custkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(custkey_vector));
		auto phones = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(phone_vector));
		auto acctbals = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(acctbal_vector));
		auto &custkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(custkey_vector));
		auto &phone_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(phone_vector));
		auto &acctbal_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(acctbal_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			profile.rows_scanned++;
			const auto custkey_index = custkey_sel.get_index(row);
			const auto phone_index = phone_sel.get_index(row);
			const auto acctbal_index = acctbal_sel.get_index(row);
			if (!custkey_validity.RowIsValid(custkey_index) || !phone_validity.RowIsValid(phone_index) ||
			    !acctbal_validity.RowIsValid(acctbal_index)) {
				profile.fallback_rows++;
				continue;
			}
			uint8_t country_code = 0;
			if (!CountryAllowed(phones[phone_index], setup, country_code)) {
				continue;
			}
			process_row(custkeys[custkey_index], country_code, acctbals[acctbal_index]);
		}
		return;
	}

	profile.fallback_rows += chunk.size();
	duckdb::UnifiedVectorFormat custkey_format;
	duckdb::UnifiedVectorFormat phone_format;
	duckdb::UnifiedVectorFormat acctbal_format;
	custkey_vector.ToUnifiedFormat(chunk.size(), custkey_format);
	phone_vector.ToUnifiedFormat(chunk.size(), phone_format);
	acctbal_vector.ToUnifiedFormat(chunk.size(), acctbal_format);
	auto custkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(custkey_format);
	auto acctbals = duckdb::UnifiedVectorFormat::GetData<int64_t>(acctbal_format);
	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		profile.rows_scanned++;
		const auto custkey_index = custkey_format.sel->get_index(row);
		const auto phone_index = phone_format.sel->get_index(row);
		const auto acctbal_index = acctbal_format.sel->get_index(row);
		if ((!custkey_format.validity.AllValid() && !custkey_format.validity.RowIsValidUnsafe(custkey_index)) ||
		    (!phone_format.validity.AllValid() && !phone_format.validity.RowIsValidUnsafe(phone_index)) ||
		    (!acctbal_format.validity.AllValid() && !acctbal_format.validity.RowIsValidUnsafe(acctbal_index))) {
			continue;
		}
		const auto phone = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(phone_format)[phone_index];
		uint8_t country_code = 0;
		if (!CountryAllowed(phone, setup, country_code)) {
			continue;
		}
		process_row(custkeys[custkey_index], country_code, acctbals[acctbal_index]);
	}
}

void PrintQ22ParallelProfile(const Q22ParallelProfile &profile) {
	std::cerr << "q22_parallel_profile"
	          << " prefix_setup_ns=" << profile.prefix_setup_ns << " orders_scan_ns=" << profile.orders_scan_ns
	          << " orders_merge_ns=" << profile.orders_merge_ns << " customer_pass1_ns=" << profile.customer_pass1_ns
	          << " average_reduce_ns=" << profile.average_reduce_ns << " customer_pass2_ns=" << profile.customer_pass2_ns
	          << " prefix_merge_ns=" << profile.prefix_merge_ns << " output_ns=" << profile.output_ns
	          << " worker_count=" << profile.worker_count << " orders_rows_scanned=" << profile.orders_rows_scanned
	          << " orders_flat_rows=" << profile.orders_flat_rows << " orders_fallback_rows=" << profile.orders_fallback_rows
	          << " customer_pass1_rows=" << profile.customer_pass1_rows
	          << " customer_pass1_prefix=" << profile.customer_pass1_prefix
	          << " customer_pass1_positive=" << profile.customer_pass1_positive
	          << " customer_pass2_rows=" << profile.customer_pass2_rows
	          << " customer_pass2_prefix=" << profile.customer_pass2_prefix
	          << " customer_pass2_above_average=" << profile.customer_pass2_above_average
	          << " customer_pass2_survivors=" << profile.customer_pass2_survivors
	          << " customer_pass1_flat_rows=" << profile.customer_pass1_flat_rows
	          << " customer_pass2_flat_rows=" << profile.customer_pass2_flat_rows
	          << " customer_pass1_fallback_rows=" << profile.customer_pass1_fallback_rows
	          << " customer_pass2_fallback_rows=" << profile.customer_pass2_fallback_rows
	          << " positive_balance_count=" << profile.positive_balance_count
	          << " orders_bitmap_words=" << profile.orders_bitmap_words
	          << " orders_bitmap_bytes=" << profile.orders_bitmap_bytes
	          << " orders_worker_bitmap_bytes=" << profile.orders_worker_bitmap_bytes
	          << " min_worker_pass1_ns=" << profile.min_worker_pass1_ns
	          << " max_worker_pass1_ns=" << profile.max_worker_pass1_ns
	          << " median_worker_pass1_ns=" << profile.median_worker_pass1_ns
	          << " min_worker_pass2_ns=" << profile.min_worker_pass2_ns
	          << " max_worker_pass2_ns=" << profile.max_worker_pass2_ns
	          << " median_worker_pass2_ns=" << profile.median_worker_pass2_ns << "\n";
}

class Q22ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q22ParallelKernel(Q22Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 2 || !bound_inputs[0] || !bound_inputs[1]) {
			throw duckdb::InternalException("Q22 parallel expects orders and customer scans");
		}

		using clock = std::chrono::steady_clock;
		Q22ParallelProfile profile;
		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);

		const auto prefix_t0 = clock::now();
		const auto prefix_setup = BuildPrefixSetup(params);
		profile.prefix_setup_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - prefix_t0).count();

		struct OrdersWorkerState {
			CustomerHasOrdersBitmap bitmap;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<OrdersWorkerState>> order_workers(static_cast<size_t>(worker_count));

		const auto orders_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[0], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     auto &worker = order_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateOrdersChunk(chunk, worker.bitmap, worker.stats);
		                     });
		profile.orders_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_t0).count();

		const auto orders_merge_t0 = clock::now();
		CustomerHasOrdersBitmap has_orders;
		for (const auto &worker : order_workers) {
			profile.orders_rows_scanned += worker.value.stats.rows_scanned;
			profile.orders_flat_rows += worker.value.stats.flat_rows;
			profile.orders_fallback_rows += worker.value.stats.fallback_rows;
			profile.orders_worker_bitmap_bytes += worker.value.bitmap.LogicalBytes();
			has_orders.MergeFrom(worker.value.bitmap);
		}
		profile.orders_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - orders_merge_t0).count();
		profile.orders_bitmap_words = has_orders.WordCount();
		profile.orders_bitmap_bytes = has_orders.LogicalBytes();

		struct AverageWorkerState {
			AveragePartial partial;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<AverageWorkerState>> average_workers(static_cast<size_t>(worker_count));

		const auto pass1_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto chunk_t0 = clock::now();
			                     auto &worker = average_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateAverageChunk(chunk, prefix_setup, worker.partial, worker.stats);
			                     worker.stats.process_ns +=
			                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0).count();
		                     });
		profile.customer_pass1_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - pass1_t0).count();

		const auto average_reduce_t0 = clock::now();
		AveragePartial average_state;
		std::vector<int64_t> pass1_worker_ns(static_cast<size_t>(worker_count), 0);
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			const auto &worker = average_workers[worker_id].value;
			MergeAveragePartial(average_state, worker.partial);
			profile.customer_pass1_rows += worker.stats.rows_scanned;
			profile.customer_pass1_prefix += worker.stats.prefix_matched;
			profile.customer_pass1_positive += worker.stats.positive_matched;
			profile.customer_pass1_flat_rows += worker.stats.flat_rows;
			profile.customer_pass1_fallback_rows += worker.stats.fallback_rows;
			pass1_worker_ns[worker_id] = worker.stats.process_ns;
		}
		profile.positive_balance_count = static_cast<size_t>(average_state.positive_balance_count);
		profile.average_reduce_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - average_reduce_t0).count();

		if (!pass1_worker_ns.empty()) {
			profile.min_worker_pass1_ns = static_cast<size_t>(*std::min_element(pass1_worker_ns.begin(), pass1_worker_ns.end()));
			profile.max_worker_pass1_ns = static_cast<size_t>(*std::max_element(pass1_worker_ns.begin(), pass1_worker_ns.end()));
			auto sorted = pass1_worker_ns;
			std::sort(sorted.begin(), sorted.end());
			profile.median_worker_pass1_ns = static_cast<size_t>(sorted[sorted.size() / 2]);
		}

		struct GroupWorkerState {
			PrefixGroupPartial groups;
			PassProfile stats;
		};
		std::vector<CacheLinePadded<GroupWorkerState>> group_workers(static_cast<size_t>(worker_count));

		const auto pass2_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[1], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto chunk_t0 = clock::now();
			                     auto &worker = group_workers[static_cast<size_t>(worker_id)].value;
			                     AccumulateQualifyingChunk(chunk, prefix_setup, has_orders, average_state.positive_balance_sum,
			                                               average_state.positive_balance_count, worker.groups, worker.stats);
			                     worker.stats.process_ns +=
			                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - chunk_t0).count();
		                     });
		profile.customer_pass2_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - pass2_t0).count();

		const auto prefix_merge_t0 = clock::now();
		PrefixGroupPartial merged_groups;
		std::vector<int64_t> pass2_worker_ns(static_cast<size_t>(worker_count), 0);
		for (size_t worker_id = 0; worker_id < static_cast<size_t>(worker_count); worker_id++) {
			const auto &worker = group_workers[worker_id].value;
			MergePrefixGroups(merged_groups, worker.groups);
			profile.customer_pass2_rows += worker.stats.rows_scanned;
			profile.customer_pass2_prefix += worker.stats.prefix_matched;
			profile.customer_pass2_above_average += worker.stats.above_average;
			profile.customer_pass2_survivors += worker.stats.anti_join_survivors;
			profile.customer_pass2_flat_rows += worker.stats.flat_rows;
			profile.customer_pass2_fallback_rows += worker.stats.fallback_rows;
			pass2_worker_ns[worker_id] = worker.stats.process_ns;
		}
		profile.prefix_merge_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - prefix_merge_t0).count();

		if (!pass2_worker_ns.empty()) {
			profile.min_worker_pass2_ns = static_cast<size_t>(*std::min_element(pass2_worker_ns.begin(), pass2_worker_ns.end()));
			profile.max_worker_pass2_ns = static_cast<size_t>(*std::max_element(pass2_worker_ns.begin(), pass2_worker_ns.end()));
			auto sorted = pass2_worker_ns;
			std::sort(sorted.begin(), sorted.end());
			profile.median_worker_pass2_ns = static_cast<size_t>(sorted[sorted.size() / 2]);
		}

		const auto output_t0 = clock::now();
		output_rows.clear();
		for (size_t code = 0; code < kCountrySlotCount; code++) {
			if (merged_groups.counts[code] == 0) {
				continue;
			}
			Q22OutputRow row;
			row.country_code = prefix_setup.country_labels[code];
			row.customer_count = merged_groups.counts[code];
			row.total_balance = FromAccumulator(merged_groups.totals[code]);
			output_rows.push_back(std::move(row));
		}
		std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
		profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();

		if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
			PrintQ22ParallelProfile(profile);
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
			output.SetValue(0, row, duckdb::Value(entry.country_code));
			output.SetValue(1, row, duckdb::Value::BIGINT(entry.customer_count));
			output.SetValue(2, row, duckdb::Value::DECIMAL(entry.total_balance, 38, 2));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	Q22Parameters params;
	std::vector<Q22OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ22ParallelKernel(const Q22Parameters &params) {
	return duckdb::make_uniq<Q22ParallelKernel>(params);
}

} // namespace accelerator_codegen
