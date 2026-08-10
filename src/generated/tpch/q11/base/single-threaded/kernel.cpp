#include "tpch/q11/base/query.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultPartCapacity = 200001;

#if defined(__SIZEOF_INT128__)
using q11_acc_t = __int128_t;

duckdb::hugeint_t FromAccumulator(q11_acc_t value) {
	return duckdb::hugeint_t(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value));
}

void AddValue(q11_acc_t &accumulator, int64_t supplycost_raw, int64_t availqty) {
	accumulator += static_cast<q11_acc_t>(supplycost_raw) * static_cast<q11_acc_t>(availqty);
}
#else
using q11_acc_t = duckdb::hugeint_t;

duckdb::hugeint_t FromAccumulator(const q11_acc_t &value) {
	return value;
}

void AddValue(q11_acc_t &accumulator, int64_t supplycost_raw, int64_t availqty) {
	duckdb::hugeint_t cost_value;
	duckdb::hugeint_t qty_value;
	duckdb::hugeint_t product_value;
	if (!duckdb::Hugeint::TryConvert(supplycost_raw, cost_value) ||
	    !duckdb::Hugeint::TryConvert(availqty, qty_value) ||
	    !duckdb::Hugeint::TryMultiply(cost_value, qty_value, product_value) ||
	    !duckdb::Hugeint::TryAddInPlace(accumulator, product_value)) {
		throw duckdb::OutOfRangeException("Overflow while accumulating Q11 value");
	}
}
#endif

struct Q11RuntimeConfig {
	bool supplier_flat_fast = true;
	bool supplier_dict_fast = true;
	bool partsupp_flat_fast = true;
	bool partsupp_dict_fast = true;
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

struct Q11OutputRow {
	int64_t partkey = 0;
	duckdb::hugeint_t value;
};

bool BetterOutputRow(const Q11OutputRow &lhs, const Q11OutputRow &rhs) {
	if (duckdb::Hugeint::GreaterThan(lhs.value, rhs.value)) {
		return true;
	}
	if (duckdb::Hugeint::GreaterThan(rhs.value, lhs.value)) {
		return false;
	}
	return lhs.partkey < rhs.partkey;
}

bool ValueGreaterThan(const duckdb::hugeint_t &lhs, const duckdb::Value &rhs) {
	const auto lhs_value = duckdb::Value::DECIMAL(lhs, 38, 2);
	return lhs_value > rhs;
}

duckdb::Value ComputeThreshold(const duckdb::hugeint_t &total_value, double fraction) {
	const auto total_decimal = duckdb::Value::DECIMAL(total_value, 38, 2);
	const auto total_as_double = total_decimal.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
	return duckdb::Value::DOUBLE(total_as_double * fraction);
}

class Q11Kernel final : public KernelInstance {
public:
	explicit Q11Kernel(Q11Parameters params_p, Q11ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config() {
		supplier_qualifying.assign(static_cast<size_t>(kDefaultSupplierCapacity), 0);
		if (profile) {
			profile->used_dense_state = true;
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q11InputId>(input_id)) {
		case Q11InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q11InputId::PARTSUPP:
			ConsumePartsupp(input);
			return;
		default:
			throw duckdb::InternalException("Q11 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		duckdb::Value threshold_value;
		if (has_total_value) {
			threshold_value = ComputeThreshold(FromAccumulator(total_value), params.fraction);
		}

		output_rows.clear();
		output_rows.reserve(active_parts.size());
		for (const auto partkey : active_parts) {
			if (partkey < 0 || partkey >= part_capacity) {
				continue;
			}
			const auto part_value = FromAccumulator(value_by_part[static_cast<size_t>(partkey)]);
			if (!has_total_value || !ValueGreaterThan(part_value, threshold_value)) {
				continue;
			}
			Q11OutputRow row;
			row.partkey = partkey;
			row.value = part_value;
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
			output.SetValue(0, row, duckdb::Value::BIGINT(entry.partkey));
			output.SetValue(1, row, duckdb::Value::DECIMAL(entry.value, 38, 2));
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
		if (required <= static_cast<int64_t>(supplier_qualifying.size())) {
			return;
		}
		size_t target = static_cast<size_t>(required);
		if (!supplier_qualifying.empty()) {
			const size_t doubled = supplier_qualifying.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultSupplierCapacity));
		}
		supplier_qualifying.resize(target, 0);
	}

	void EnsurePartCapacity(int64_t partkey) {
		if (partkey < 0) {
			return;
		}
		const auto required = partkey + 1;
		if (required <= part_capacity) {
			return;
		}
		int64_t target = required;
		if (part_capacity > 0) {
			const int64_t doubled = part_capacity > (std::numeric_limits<int64_t>::max() / 2)
			                              ? std::numeric_limits<int64_t>::max()
			                              : part_capacity * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, kDefaultPartCapacity);
		}
		value_by_part.resize(static_cast<size_t>(target), 0);
		part_has_value.resize(static_cast<size_t>(target), 0);
		part_capacity = target;
	}

	void MarkSupplierQualifying(int64_t suppkey) {
		EnsureSupplierCapacity(suppkey);
		supplier_qualifying[static_cast<size_t>(suppkey)] = 1;
		has_qualifying_suppliers = true;
	}

	void AddPartValue(int64_t partkey, int64_t supplycost_raw, int64_t availqty) {
		EnsurePartCapacity(partkey);
		const auto index = static_cast<size_t>(partkey);
		if (!part_has_value[index]) {
			part_has_value[index] = 1;
			active_parts.push_back(partkey);
		}
		AddValue(value_by_part[index], supplycost_raw, availqty);
		AddValue(total_value, supplycost_raw, availqty);
		has_total_value = true;
	}

	bool SupplierQualifies(int64_t suppkey) const {
		if (suppkey < 0 || suppkey >= static_cast<int64_t>(supplier_qualifying.size())) {
			return false;
		}
		return supplier_qualifying[static_cast<size_t>(suppkey)] != 0;
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		if (params.target_nationkey < 0) {
			return;
		}
		auto &suppkey_vector = input.data[Q11SupplierChunkLayout::SUPPKEY_INDEX];
		if (config.supplier_flat_fast && IsFlatAllValid(suppkey_vector)) {
			if (profile) {
				profile->supplier_flat_fast_rows += input.size();
			}
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->supplier_rows_scanned += 1;
					profile->supplier_rows_matched += 1;
				}
				MarkSupplierQualifying(suppkeys[row]);
			}
			return;
		}

		if (config.supplier_dict_fast && IsDictionaryOverFlat(suppkey_vector)) {
			if (profile) {
				profile->supplier_dict_fast_rows += input.size();
			}
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (profile) {
					profile->supplier_rows_scanned += 1;
				}
				const auto suppkey_index = suppkey_sel.get_index(row);
				if (!suppkey_validity.RowIsValid(suppkey_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				if (profile) {
					profile->supplier_rows_matched += 1;
				}
				MarkSupplierQualifying(suppkeys[suppkey_index]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat suppkey_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			if (profile) {
				profile->supplier_rows_scanned += 1;
			}
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			if (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			if (profile) {
				profile->supplier_rows_matched += 1;
			}
			MarkSupplierQualifying(suppkeys[suppkey_index]);
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		if (!has_qualifying_suppliers) {
			return;
		}
		if (profile) {
			profile->partsupp_rows_scanned += input.size();
		}
		auto &partkey_vector = input.data[Q11PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q11PartsuppChunkLayout::SUPPKEY_INDEX];
		auto &availqty_vector = input.data[Q11PartsuppChunkLayout::AVAILQTY_INDEX];
		auto &supplycost_vector = input.data[Q11PartsuppChunkLayout::SUPPLYCOST_INDEX];

		if (config.partsupp_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(availqty_vector) && IsFlatAllValid(supplycost_vector)) {
			if (profile) {
				profile->partsupp_flat_fast_rows += input.size();
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto availqtys = duckdb::FlatVector::GetData<int64_t>(availqty_vector);
			auto supplycosts = duckdb::FlatVector::GetData<int64_t>(supplycost_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (!SupplierQualifies(suppkeys[row])) {
					continue;
				}
				AddPartValue(partkeys[row], supplycosts[row], availqtys[row]);
				if (profile) {
					profile->partsupp_rows_matched += 1;
				}
			}
			return;
		}

		if (config.partsupp_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(suppkey_vector) &&
		    IsDictionaryOverFlat(availqty_vector) && IsDictionaryOverFlat(supplycost_vector)) {
			if (profile) {
				profile->partsupp_dict_fast_rows += input.size();
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &availqty_sel = duckdb::DictionaryVector::SelVector(availqty_vector);
			auto &supplycost_sel = duckdb::DictionaryVector::SelVector(supplycost_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto availqtys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(availqty_vector));
			auto supplycosts = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(supplycost_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &availqty_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(availqty_vector));
			auto &supplycost_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(supplycost_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto availqty_index = availqty_sel.get_index(row);
				const auto supplycost_index = supplycost_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !suppkey_validity.RowIsValid(suppkey_index) ||
				    !availqty_validity.RowIsValid(availqty_index) || !supplycost_validity.RowIsValid(supplycost_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				if (!SupplierQualifies(suppkeys[suppkey_index])) {
					continue;
				}
				AddPartValue(partkeys[partkey_index], supplycosts[supplycost_index], availqtys[availqty_index]);
				if (profile) {
					profile->partsupp_rows_matched += 1;
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat availqty_format;
		duckdb::UnifiedVectorFormat supplycost_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		availqty_vector.ToUnifiedFormat(input.size(), availqty_format);
		supplycost_vector.ToUnifiedFormat(input.size(), supplycost_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);
		auto supplycosts = duckdb::UnifiedVectorFormat::GetData<int64_t>(supplycost_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto availqty_index = availqty_format.sel->get_index(row);
			const auto supplycost_index = supplycost_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!availqty_format.validity.AllValid() && !availqty_format.validity.RowIsValidUnsafe(availqty_index)) ||
			    (!supplycost_format.validity.AllValid() &&
			     !supplycost_format.validity.RowIsValidUnsafe(supplycost_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			if (!SupplierQualifies(suppkeys[suppkey_index])) {
				continue;
			}
			AddPartValue(partkeys[partkey_index], supplycosts[supplycost_index], availqtys[availqty_index]);
			if (profile) {
				profile->partsupp_rows_matched += 1;
			}
		}
	}

	Q11Parameters params;
	Q11ProfileCounters *profile;
	Q11RuntimeConfig config;
	bool has_qualifying_suppliers = false;
	std::vector<uint8_t> supplier_qualifying;
	std::vector<q11_acc_t> value_by_part;
	std::vector<uint8_t> part_has_value;
	std::vector<int64_t> active_parts;
	int64_t part_capacity = 0;
	q11_acc_t total_value = 0;
	bool has_total_value = false;
	std::vector<Q11OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ11Kernel(const Q11Parameters &params) {
	return duckdb::make_uniq<Q11Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ11ProfilingKernel(const Q11Parameters &params, Q11ProfileCounters *profile) {
	return duckdb::make_uniq<Q11Kernel>(params, profile);
}

} // namespace accelerator_codegen
