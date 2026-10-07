#include "tpch/q20/duckdb/base/query.hpp"
#include "tpch/q20/duckdb/base/q20_common.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;

struct Q20RuntimeConfig {
	bool lineitem_flat_fast = true;
	bool lineitem_dict_fast = true;
};

bool IsFlatAllValid(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
	       duckdb::FlatVector::Validity(vector).AllValid();
}

bool IsDictionaryOverFlatAllValid(duckdb::Vector &vector) {
	if (vector.GetVectorType() != duckdb::VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	auto &child = duckdb::DictionaryVector::Child(vector);
	return child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && duckdb::FlatVector::Validity(child).AllValid();
}

template <class T>
const T *GetFastPathData(duckdb::Vector &vector) {
	if (vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
		return duckdb::FlatVector::GetData<T>(vector);
	}
	return duckdb::FlatVector::GetData<T>(duckdb::DictionaryVector::Child(vector));
}

duckdb::idx_t GetFastPathIndex(duckdb::Vector &vector, duckdb::idx_t row) {
	if (vector.GetVectorType() == duckdb::VectorType::DICTIONARY_VECTOR) {
		return duckdb::DictionaryVector::SelVector(vector).get_index(row);
	}
	return row;
}

bool SqlLikeAt(const char *text, duckdb::idx_t text_len, const char *pattern, duckdb::idx_t pattern_len,
               duckdb::idx_t text_pos, duckdb::idx_t pattern_pos) {
	while (pattern_pos < pattern_len) {
		const char pattern_ch = pattern[pattern_pos];
		if (pattern_ch == '%') {
			if (pattern_pos + 1 == pattern_len) {
				return true;
			}
			for (duckdb::idx_t start = text_pos; start <= text_len; start++) {
				if (SqlLikeAt(text, text_len, pattern, pattern_len, start, pattern_pos + 1)) {
					return true;
				}
			}
			return false;
		}
		if (pattern_ch == '_') {
			if (text_pos >= text_len) {
				return false;
			}
			text_pos++;
			pattern_pos++;
			continue;
		}
		if (text_pos >= text_len || text[text_pos] != pattern_ch) {
			return false;
		}
		text_pos++;
		pattern_pos++;
	}
	return text_pos == text_len;
}

bool SqlLike(const duckdb::string_t &text, const std::string &pattern) {
	return SqlLikeAt(text.GetData(), text.GetSize(), pattern.data(), pattern.size(), 0, 0);
}

struct PrefixLikeMatcher {
	std::string prefix;
	bool enabled = false;

	explicit PrefixLikeMatcher(const std::string &pattern) {
		if (pattern.empty() || pattern.back() != '%') {
			return;
		}
		if (pattern.find('%', 0) != pattern.size() - 1) {
			return;
		}
		prefix = pattern.substr(0, pattern.size() - 1);
		enabled = !prefix.empty();
	}

	bool Matches(const duckdb::string_t &text) const {
		if (!enabled) {
			return false;
		}
		if (text.GetSize() < prefix.size()) {
			return false;
		}
		return std::memcmp(text.GetData(), prefix.data(), prefix.size()) == 0;
	}
};

inline uint64_t MakeAggKey(int64_t partkey, int64_t suppkey) {
	return (static_cast<uint64_t>(partkey) << 20) | static_cast<uint64_t>(suppkey);
}

struct Q20OutputRow {
	std::string name;
	std::string address;
};

bool BetterOutputRow(const Q20OutputRow &lhs, const Q20OutputRow &rhs) {
	return lhs.name < rhs.name;
}

bool AvailQtyGreaterThanHalfSum(int64_t availqty, int64_t sum_qty_raw) {
#if defined(__SIZEOF_INT128__)
	return static_cast<__int128_t>(availqty) * 200 > static_cast<__int128_t>(sum_qty_raw);
#else
	return availqty > sum_qty_raw / 200;
#endif
}

std::string ReadString(const duckdb::string_t &value) {
	return std::string(value.GetData(), value.GetData() + value.GetSize());
}

class Q20Kernel final : public KernelInstance {
public:
	explicit Q20Kernel(Q20Parameters params_p)
	    : params(std::move(params_p)), part_name_matcher(params.part_name_pattern),
	      fallback_part_name_pattern(params.part_name_pattern) {
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q20InputId>(input_id)) {
		case Q20InputId::PART:
			ConsumePart(input);
			return;
		case Q20InputId::LINEITEM:
			ConsumeLineitem(input);
			return;
		case Q20InputId::PARTSUPP:
			ConsumePartsupp(input);
			return;
		case Q20InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		default:
			throw duckdb::InternalException("Q20 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		output_rows.clear();
		output_rows.reserve(qualifying_suppliers.size());
		for (const auto suppkey : qualifying_suppliers) {
			const auto supplier_it = suppliers.find(suppkey);
			if (supplier_it == suppliers.end()) {
				continue;
			}
			Q20OutputRow row;
			row.name = supplier_it->second.name;
			row.address = supplier_it->second.address;
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
			output.SetValue(1, row, duckdb::Value(entry.address));
		}
		output.SetCardinality(count);
		output_offset += count;
		return true;
	}

private:
	struct SupplierRow {
		std::string name;
		std::string address;
	};

	bool PartNameMatches(const duckdb::string_t &name) const {
		if (part_name_matcher.enabled) {
			return part_name_matcher.Matches(name);
		}
		return SqlLike(name, fallback_part_name_pattern);
	}

	void ConsumeLineitemFast(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q20LineitemChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q20LineitemChunkLayout::SUPPKEY_INDEX];
		auto &quantity_vector = input.data[Q20LineitemChunkLayout::QUANTITY_INDEX];
		const auto count = input.size();
		auto partkeys = GetFastPathData<int64_t>(partkey_vector);
		auto suppkeys = GetFastPathData<int64_t>(suppkey_vector);
		auto quantities = GetFastPathData<int64_t>(quantity_vector);
		if (partkey_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR && qualifying_parts.SimdUsable() &&
		    suppkey_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR &&
		    quantity_vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (sel_buf.size() < count + simd::kSelSlack) {
				sel_buf.resize(count + simd::kSelSlack);
			}
			const size_t picked =
			    q20::SelectFlagged(qualifying_parts.Data(), qualifying_parts.Size(), partkeys, count, sel_buf.data());
			for (size_t k = 0; k < picked; k++) {
				const uint32_t row = sel_buf[k];
				quantity_by_part_supplier.Add(partkeys[row], suppkeys[row], quantities[row]);
			}
			return;
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto partkey = partkeys[GetFastPathIndex(partkey_vector, row)];
			if (!qualifying_parts.Contains(partkey)) {
				continue;
			}
			const auto suppkey = suppkeys[GetFastPathIndex(suppkey_vector, row)];
			const auto quantity = quantities[GetFastPathIndex(quantity_vector, row)];
			quantity_by_part_supplier.Add(partkey, suppkey, quantity);
		}
	}

	void ConsumeLineitem(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q20LineitemChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q20LineitemChunkLayout::SUPPKEY_INDEX];
		auto &quantity_vector = input.data[Q20LineitemChunkLayout::QUANTITY_INDEX];

		if (config.lineitem_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) &&
		    IsFlatAllValid(quantity_vector)) {
			ConsumeLineitemFast(input);
			return;
		}

		if (config.lineitem_dict_fast && IsDictionaryOverFlatAllValid(partkey_vector) &&
		    IsDictionaryOverFlatAllValid(suppkey_vector) && IsDictionaryOverFlatAllValid(quantity_vector)) {
			ConsumeLineitemFast(input);
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat quantity_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		quantity_vector.ToUnifiedFormat(input.size(), quantity_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto quantities = duckdb::UnifiedVectorFormat::GetData<int64_t>(quantity_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto quantity_index = quantity_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!quantity_format.validity.AllValid() && !quantity_format.validity.RowIsValidUnsafe(quantity_index))) {
				continue;
			}
			const auto partkey = partkeys[partkey_index];
			if (!qualifying_parts.Contains(partkey)) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			quantity_by_part_supplier.Add(partkey, suppkey, quantities[quantity_index]);
		}
	}

	void ConsumePart(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q20PartChunkLayout::PARTKEY_INDEX];
		auto &name_vector = input.data[Q20PartChunkLayout::NAME_INDEX];
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(name_vector)) {
			const auto count = input.size();
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto names = duckdb::FlatVector::GetData<duckdb::string_t>(name_vector);
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!PartNameMatches(names[row])) {
					continue;
				}
				qualifying_parts.Insert(partkeys[row]);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat name_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		name_vector.ToUnifiedFormat(input.size(), name_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index))) {
				continue;
			}
			const auto name =
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index];
			if (!PartNameMatches(name)) {
				continue;
			}
			qualifying_parts.Insert(partkeys[partkey_index]);
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q20PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q20PartsuppChunkLayout::SUPPKEY_INDEX];
		auto &availqty_vector = input.data[Q20PartsuppChunkLayout::AVAILQTY_INDEX];
		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(availqty_vector)) {
			const auto count = input.size();
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto availqtys = duckdb::FlatVector::GetData<int64_t>(availqty_vector);
			if (sel_buf.size() < count + simd::kSelSlack) {
				sel_buf.resize(count + simd::kSelSlack);
			}
			const bool simd_ok = qualifying_parts.SimdUsable();
			size_t picked = 0;
			if (simd_ok) {
				picked = q20::SelectFlagged(qualifying_parts.Data(), qualifying_parts.Size(), partkeys, count,
				                            sel_buf.data());
			} else {
				for (duckdb::idx_t row = 0; row < count; row++) {
					sel_buf[picked] = static_cast<uint32_t>(row);
					picked += qualifying_parts.Contains(partkeys[row]);
				}
			}
			for (size_t k = 0; k < picked; k++) {
				const uint32_t row = sel_buf[k];
				const auto partkey = partkeys[row];
				const auto suppkey = suppkeys[row];
				const int64_t *sum = quantity_by_part_supplier.Find(partkey, suppkey);
				if (sum == nullptr) {
					continue;
				}
				if (!AvailQtyGreaterThanHalfSum(availqtys[row], *sum)) {
					continue;
				}
				qualifying_suppliers.insert(suppkey);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat availqty_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		availqty_vector.ToUnifiedFormat(input.size(), availqty_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto availqtys = duckdb::UnifiedVectorFormat::GetData<int64_t>(availqty_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto availqty_index = availqty_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!availqty_format.validity.AllValid() && !availqty_format.validity.RowIsValidUnsafe(availqty_index))) {
				continue;
			}
			const auto partkey = partkeys[partkey_index];
			if (!qualifying_parts.Contains(partkey)) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			const int64_t *agg_sum = quantity_by_part_supplier.Find(partkey, suppkey);
			if (agg_sum == nullptr) {
				continue;
			}
			if (!AvailQtyGreaterThanHalfSum(availqtys[availqty_index], *agg_sum)) {
				continue;
			}
			qualifying_suppliers.insert(suppkey);
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat address_format;
		input.data[Q20SupplierChunkLayout::SUPPKEY_INDEX].ToUnifiedFormat(input.size(), suppkey_format);
		input.data[Q20SupplierChunkLayout::NAME_INDEX].ToUnifiedFormat(input.size(), name_format);
		input.data[Q20SupplierChunkLayout::ADDRESS_INDEX].ToUnifiedFormat(input.size(), address_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto address_index = address_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) ||
			    (!address_format.validity.AllValid() && !address_format.validity.RowIsValidUnsafe(address_index))) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (qualifying_suppliers.find(suppkey) == qualifying_suppliers.end()) {
				continue;
			}
			SupplierRow supplier;
			supplier.name = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format)[name_index]);
			supplier.address = ReadString(
			    duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(address_format)[address_index]);
			suppliers.emplace(suppkey, std::move(supplier));
		}
	}

	Q20Parameters params;
	Q20RuntimeConfig config;
	PrefixLikeMatcher part_name_matcher;
	std::string fallback_part_name_pattern;
	q20::AggTable quantity_by_part_supplier;
	q20::PartFlags qualifying_parts;
	std::vector<uint32_t> sel_buf;
	std::unordered_set<int64_t> qualifying_suppliers;
	std::unordered_map<int64_t, SupplierRow> suppliers;
	std::vector<Q20OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ20Kernel(const Q20Parameters &params) {
	return duckdb::make_uniq<Q20Kernel>(params);
}

} // namespace accelerator_codegen
