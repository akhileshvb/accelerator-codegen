#include "accelerator_codegen/tpch_q16.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr char kComplaintPattern[] = "%Customer%Complaints%";
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultPartCapacity = 200001;
constexpr int32_t kMaxPartSize = 50;

struct Q16RuntimeConfig {
	bool supplier_flat_fast = true;
	bool supplier_dict_fast = true;
	bool part_flat_fast = true;
	bool part_dict_fast = true;
	bool partsupp_flat_fast = true;
	bool partsupp_dict_fast = true;
	bool two_part_like = true;
	bool prefix_like = true;
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

bool SqlLike(const duckdb::string_t &text, const char *pattern) {
	const auto pattern_len = std::strlen(pattern);
	return SqlLikeAt(text.GetData(), text.GetSize(), pattern, pattern_len, 0, 0);
}

bool SqlLike(const duckdb::string_t &text, const std::string &pattern) {
	return SqlLikeAt(text.GetData(), text.GetSize(), pattern.data(), pattern.size(), 0, 0);
}

bool StringEquals(const duckdb::string_t &lhs, const std::string &rhs) {
	return lhs.GetSize() == rhs.size() && std::memcmp(lhs.GetData(), rhs.data(), rhs.size()) == 0;
}

struct TwoPartLikeMatcher {
	std::string part1;
	std::string part2;
	bool enabled = false;

	explicit TwoPartLikeMatcher(const char *pattern) {
		const std::string_view pattern_view(pattern);
		if (pattern_view.size() < 4 || pattern_view.front() != '%' || pattern_view.back() != '%') {
			return;
		}
		const auto first_end = pattern_view.find('%', 1);
		if (first_end == std::string_view::npos || first_end == 1) {
			return;
		}
		const auto second_start = first_end + 1;
		const auto second_end = pattern_view.find('%', second_start);
		if (second_end == std::string_view::npos || second_end == second_start) {
			return;
		}
		if (pattern_view.find('%', second_end + 1) != std::string_view::npos) {
			return;
		}
		part1 = std::string(pattern_view.substr(1, first_end - 1));
		part2 = std::string(pattern_view.substr(second_start, second_end - second_start));
		enabled = !part1.empty() && !part2.empty();
	}

	bool Matches(const duckdb::string_t &text) const {
		if (!enabled) {
			return false;
		}
		const std::string_view haystack(text.GetData(), text.GetSize());
		size_t pos = 0;
		while (pos + part1.size() <= haystack.size()) {
			const auto found = haystack.find(part1, pos);
			if (found == std::string_view::npos) {
				return false;
			}
			if (haystack.find(part2, found + part1.size()) != std::string_view::npos) {
				return true;
			}
			pos = found + 1;
		}
		return false;
	}
};

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

class StringInterner {
public:
	uint16_t Intern(const duckdb::string_t &value) {
		const std::string_view probe(value.GetData(), value.GetSize());
		for (uint16_t id = 0; id < values.size(); id++) {
			const auto &stored = values[id];
			if (stored.size() == probe.size() && std::memcmp(stored.data(), probe.data(), probe.size()) == 0) {
				return id;
			}
		}
		const uint16_t id = static_cast<uint16_t>(values.size());
		values.emplace_back(probe);
		return id;
	}

	const std::string &Get(uint16_t id) const {
		return values[id];
	}

private:
	std::vector<std::string> values;
};

struct PartGroup {
	std::string brand;
	std::string type;
	int32_t size = 0;
};

struct CompactGroupKey {
	uint16_t brand_id = 0;
	uint16_t type_id = 0;
	int32_t size = 0;

	bool operator==(const CompactGroupKey &other) const {
		return brand_id == other.brand_id && type_id == other.type_id && size == other.size;
	}
};

struct CompactGroupKeyHash {
	size_t operator()(const CompactGroupKey &key) const {
		return (static_cast<size_t>(key.brand_id) << 32) ^ (static_cast<size_t>(key.type_id) << 16) ^
		       static_cast<size_t>(key.size);
	}
};

struct GroupSuppPair {
	int32_t group_id = 0;
	int32_t suppkey = 0;
};

struct Q16OutputRow {
	std::string brand;
	std::string type;
	int32_t size = 0;
	int64_t supplier_cnt = 0;
};

bool BetterOutputRow(const Q16OutputRow &lhs, const Q16OutputRow &rhs) {
	if (lhs.supplier_cnt != rhs.supplier_cnt) {
		return lhs.supplier_cnt > rhs.supplier_cnt;
	}
	if (lhs.brand != rhs.brand) {
		return lhs.brand < rhs.brand;
	}
	if (lhs.type != rhs.type) {
		return lhs.type < rhs.type;
	}
	return lhs.size < rhs.size;
}

bool LessGroupSuppPair(const GroupSuppPair &lhs, const GroupSuppPair &rhs) {
	if (lhs.group_id != rhs.group_id) {
		return lhs.group_id < rhs.group_id;
	}
	return lhs.suppkey < rhs.suppkey;
}

class Q16Kernel final : public KernelInstance {
public:
	explicit Q16Kernel(Q16Parameters params_p, Q16ProfileCounters *profile_p = nullptr)
	    : params(std::move(params_p)), profile(profile_p), config(), complaint_matcher(kComplaintPattern),
	      type_prefix_matcher(params.type_pattern) {
		allowed_sizes.fill(0);
		for (const auto size : params.sizes) {
			if (size >= 1 && size <= kMaxPartSize) {
				allowed_sizes[static_cast<size_t>(size)] = 1;
			}
		}
		excluded_suppliers.assign(static_cast<size_t>(kDefaultSupplierCapacity), 0);
		part_group_slot.assign(static_cast<size_t>(kDefaultPartCapacity), 0);
		group_supplier_pairs.reserve(1 << 20);
		if (profile) {
			profile->used_dense_state = true;
		}
		if (config.two_part_like && complaint_matcher.enabled) {
			if (profile) {
				profile->used_two_part_like = true;
			}
		}
		if (config.prefix_like && type_prefix_matcher.enabled) {
			if (profile) {
				profile->used_prefix_like = true;
			}
		}
	}

	void ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) override {
		switch (static_cast<Q16InputId>(input_id)) {
		case Q16InputId::SUPPLIER:
			ConsumeSupplier(input);
			return;
		case Q16InputId::PART:
			ConsumePart(input);
			return;
		case Q16InputId::PARTSUPP:
			ConsumePartsupp(input);
			return;
		default:
			throw duckdb::InternalException("Q16 kernel received unexpected input id %llu",
			                                static_cast<unsigned long long>(input_id));
		}
	}

	void Finalize() override {
		std::vector<int64_t> group_counts(groups.size(), 0);
		if (!group_supplier_pairs.empty()) {
			std::sort(group_supplier_pairs.begin(), group_supplier_pairs.end(), LessGroupSuppPair);
			for (size_t index = 0; index < group_supplier_pairs.size();) {
				const auto group_id = static_cast<size_t>(group_supplier_pairs[index].group_id);
				if (group_id < group_counts.size()) {
					group_counts[group_id] += 1;
				}
				const auto current_group = group_supplier_pairs[index].group_id;
				const auto current_suppkey = group_supplier_pairs[index].suppkey;
				index += 1;
				while (index < group_supplier_pairs.size() && group_supplier_pairs[index].group_id == current_group &&
				       group_supplier_pairs[index].suppkey == current_suppkey) {
					index += 1;
				}
			}
		}

		output_rows.clear();
		output_rows.reserve(groups.size());
		for (size_t group_id = 0; group_id < groups.size(); group_id++) {
			Q16OutputRow row;
			row.brand = groups[group_id].brand;
			row.type = groups[group_id].type;
			row.size = groups[group_id].size;
			row.supplier_cnt = group_counts[group_id];
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
			output.SetValue(0, row, duckdb::Value(entry.brand));
			output.SetValue(1, row, duckdb::Value(entry.type));
			output.SetValue(2, row, duckdb::Value::INTEGER(entry.size));
			output.SetValue(3, row, duckdb::Value::BIGINT(entry.supplier_cnt));
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
		const auto required = static_cast<size_t>(suppkey) + 1;
		if (required <= excluded_suppliers.size()) {
			return;
		}
		size_t target = required;
		if (!excluded_suppliers.empty()) {
			const size_t doubled = excluded_suppliers.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultSupplierCapacity));
		}
		excluded_suppliers.resize(target, 0);
	}

	void EnsurePartCapacity(int64_t partkey) {
		if (partkey < 0) {
			return;
		}
		const auto required = static_cast<size_t>(partkey) + 1;
		if (required <= part_group_slot.size()) {
			return;
		}
		size_t target = required;
		if (!part_group_slot.empty()) {
			const size_t doubled = part_group_slot.size() * 2;
			if (doubled > target) {
				target = doubled;
			}
		} else {
			target = std::max(target, static_cast<size_t>(kDefaultPartCapacity));
		}
		part_group_slot.resize(target, 0);
	}

	bool IsExcludedSupplier(int64_t suppkey) const {
		if (suppkey < 0 || static_cast<size_t>(suppkey) >= excluded_suppliers.size()) {
			return false;
		}
		return excluded_suppliers[static_cast<size_t>(suppkey)] != 0;
	}

	void MarkExcludedSupplier(int64_t suppkey) {
		EnsureSupplierCapacity(suppkey);
		excluded_suppliers[static_cast<size_t>(suppkey)] = 1;
	}

	bool CommentMatchesComplaint(const duckdb::string_t &comment) const {
		if (config.two_part_like && complaint_matcher.enabled) {
			return complaint_matcher.Matches(comment);
		}
		return SqlLike(comment, kComplaintPattern);
	}

	bool TypeMatchesExcludedPrefix(const duckdb::string_t &type_value) const {
		if (config.prefix_like && type_prefix_matcher.enabled) {
			return type_prefix_matcher.Matches(type_value);
		}
		return SqlLike(type_value, params.type_pattern);
	}

	bool SizeAllowed(int32_t size) const {
		if (size < 1 || size > kMaxPartSize) {
			return false;
		}
		return allowed_sizes[static_cast<size_t>(size)] != 0;
	}

	int32_t GetOrCreateGroupId(uint16_t brand_id, uint16_t type_id, int32_t size) {
		const CompactGroupKey key {brand_id, type_id, size};
		const auto existing = group_index.find(key);
		if (existing != group_index.end()) {
			return existing->second;
		}
		const int32_t group_id = static_cast<int32_t>(groups.size());
		groups.push_back(PartGroup {brand_intern.Get(brand_id), type_intern.Get(type_id), size});
		group_index.emplace(key, group_id);
		return group_id;
	}

	void RecordGroupSupplier(int32_t group_id, int64_t suppkey) {
		if (suppkey < 0 || suppkey > std::numeric_limits<int32_t>::max()) {
			return;
		}
		group_supplier_pairs.push_back(GroupSuppPair {group_id, static_cast<int32_t>(suppkey)});
		if (profile) {
			profile->partsupp_rows_matched += 1;
		}
	}

	void QualifyPart(int64_t partkey, const duckdb::string_t &brand_value, const duckdb::string_t &type_value,
	                 int32_t size) {
		const auto group_id = GetOrCreateGroupId(brand_intern.Intern(brand_value), type_intern.Intern(type_value), size);
		EnsurePartCapacity(partkey);
		part_group_slot[static_cast<size_t>(partkey)] = group_id + 1;
		has_qualifying_parts = true;
		if (profile) {
			profile->part_rows_matched += 1;
		}
	}

	void ConsumeSupplier(duckdb::DataChunk &input) {
		auto &suppkey_vector = input.data[Q16SupplierChunkLayout::SUPPKEY_INDEX];
		auto &comment_vector = input.data[Q16SupplierChunkLayout::COMMENT_INDEX];
		if (profile) {
			profile->supplier_rows_scanned += input.size();
		}

		if (config.supplier_flat_fast && IsFlatAllValid(suppkey_vector) && IsFlatAllValid(comment_vector)) {
			if (profile) {
				profile->supplier_flat_fast_rows += input.size();
			}
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (!CommentMatchesComplaint(comments[row])) {
					continue;
				}
				MarkExcludedSupplier(suppkeys[row]);
				if (profile) {
					profile->supplier_rows_excluded += 1;
				}
			}
			return;
		}

		if (config.supplier_dict_fast && IsDictionaryOverFlat(suppkey_vector) && IsDictionaryOverFlat(comment_vector)) {
			if (profile) {
				profile->supplier_dict_fast_rows += input.size();
			}
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &comment_sel = duckdb::DictionaryVector::SelVector(comment_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(comment_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &comment_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(comment_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto comment_index = comment_sel.get_index(row);
				if (!suppkey_validity.RowIsValid(suppkey_index) || !comment_validity.RowIsValid(comment_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				if (!CommentMatchesComplaint(comments[comment_index])) {
					continue;
				}
				MarkExcludedSupplier(suppkeys[suppkey_index]);
				if (profile) {
					profile->supplier_rows_excluded += 1;
				}
			}
			return;
		}

		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat comment_format;
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		comment_vector.ToUnifiedFormat(input.size(), comment_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto comments = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto comment_index = comment_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			if (!CommentMatchesComplaint(comments[comment_index])) {
				continue;
			}
			MarkExcludedSupplier(suppkeys[suppkey_index]);
			if (profile) {
				profile->supplier_rows_excluded += 1;
			}
		}
	}

	void ConsumePart(duckdb::DataChunk &input) {
		auto &partkey_vector = input.data[Q16PartChunkLayout::PARTKEY_INDEX];
		auto &brand_vector = input.data[Q16PartChunkLayout::BRAND_INDEX];
		auto &type_vector = input.data[Q16PartChunkLayout::TYPE_INDEX];
		auto &size_vector = input.data[Q16PartChunkLayout::SIZE_INDEX];
		if (profile) {
			profile->part_rows_scanned += input.size();
		}

		if (config.part_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) &&
		    IsFlatAllValid(type_vector) && IsFlatAllValid(size_vector)) {
			if (profile) {
				profile->part_flat_fast_rows += input.size();
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
			auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
			auto sizes = duckdb::FlatVector::GetData<int32_t>(size_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				if (!SizeAllowed(sizes[row])) {
					continue;
				}
				if (StringEquals(brands[row], params.brand)) {
					continue;
				}
				if (TypeMatchesExcludedPrefix(types[row])) {
					continue;
				}
				QualifyPart(partkeys[row], brands[row], types[row], sizes[row]);
			}
			return;
		}

		if (config.part_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(brand_vector) &&
		    IsDictionaryOverFlat(type_vector) && IsDictionaryOverFlat(size_vector)) {
			if (profile) {
				profile->part_dict_fast_rows += input.size();
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &brand_sel = duckdb::DictionaryVector::SelVector(brand_vector);
			auto &type_sel = duckdb::DictionaryVector::SelVector(type_vector);
			auto &size_sel = duckdb::DictionaryVector::SelVector(size_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(brand_vector));
			auto types = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(type_vector));
			auto sizes = duckdb::FlatVector::GetData<int32_t>(duckdb::DictionaryVector::Child(size_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &brand_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(brand_vector));
			auto &type_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(type_vector));
			auto &size_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(size_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto brand_index = brand_sel.get_index(row);
				const auto type_index = type_sel.get_index(row);
				const auto size_index = size_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !brand_validity.RowIsValid(brand_index) ||
				    !type_validity.RowIsValid(type_index) || !size_validity.RowIsValid(size_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				const auto size = sizes[size_index];
				if (!SizeAllowed(size)) {
					continue;
				}
				if (StringEquals(brands[brand_index], params.brand)) {
					continue;
				}
				if (TypeMatchesExcludedPrefix(types[type_index])) {
					continue;
				}
				QualifyPart(partkeys[partkey_index], brands[brand_index], types[type_index], size);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat brand_format;
		duckdb::UnifiedVectorFormat type_format;
		duckdb::UnifiedVectorFormat size_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		brand_vector.ToUnifiedFormat(input.size(), brand_format);
		type_vector.ToUnifiedFormat(input.size(), type_format);
		size_vector.ToUnifiedFormat(input.size(), size_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto brands = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format);
		auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);
		auto sizes = duckdb::UnifiedVectorFormat::GetData<int32_t>(size_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto brand_index = brand_format.sel->get_index(row);
			const auto type_index = type_format.sel->get_index(row);
			const auto size_index = size_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
			    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index)) ||
			    (!size_format.validity.AllValid() && !size_format.validity.RowIsValidUnsafe(size_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto size = sizes[size_index];
			if (!SizeAllowed(size)) {
				continue;
			}
			if (StringEquals(brands[brand_index], params.brand)) {
				continue;
			}
			if (TypeMatchesExcludedPrefix(types[type_index])) {
				continue;
			}
			QualifyPart(partkeys[partkey_index], brands[brand_index], types[type_index], size);
		}
	}

	void ConsumePartsupp(duckdb::DataChunk &input) {
		if (!has_qualifying_parts) {
			return;
		}
		if (profile) {
			profile->partsupp_rows_scanned += input.size();
		}
		auto &partkey_vector = input.data[Q16PartsuppChunkLayout::PARTKEY_INDEX];
		auto &suppkey_vector = input.data[Q16PartsuppChunkLayout::SUPPKEY_INDEX];
		const auto part_capacity = static_cast<int64_t>(part_group_slot.size());

		if (config.partsupp_flat_fast && IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector)) {
			if (profile) {
				profile->partsupp_flat_fast_rows += input.size();
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey = partkeys[row];
				if (partkey < 0 || partkey >= part_capacity) {
					continue;
				}
				const auto group_slot = part_group_slot[static_cast<size_t>(partkey)];
				if (group_slot == 0) {
					continue;
				}
				const auto suppkey = suppkeys[row];
				if (IsExcludedSupplier(suppkey)) {
					continue;
				}
				RecordGroupSupplier(group_slot - 1, suppkey);
			}
			return;
		}

		if (config.partsupp_dict_fast && IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(suppkey_vector)) {
			if (profile) {
				profile->partsupp_dict_fast_rows += input.size();
			}
			auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto suppkey_index = suppkey_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !suppkey_validity.RowIsValid(suppkey_index)) {
					if (profile) {
						profile->generic_fallback_rows += 1;
					}
					continue;
				}
				const auto partkey = partkeys[partkey_index];
				if (partkey < 0 || partkey >= part_capacity) {
					continue;
				}
				const auto group_slot = part_group_slot[static_cast<size_t>(partkey)];
				if (group_slot == 0) {
					continue;
				}
				const auto suppkey = suppkeys[suppkey_index];
				if (IsExcludedSupplier(suppkey)) {
					continue;
				}
				RecordGroupSupplier(group_slot - 1, suppkey);
			}
			return;
		}

		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat suppkey_format;
		partkey_vector.ToUnifiedFormat(input.size(), partkey_format);
		suppkey_vector.ToUnifiedFormat(input.size(), suppkey_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index))) {
				if (profile) {
					profile->generic_fallback_rows += 1;
				}
				continue;
			}
			const auto partkey = partkeys[partkey_index];
			if (partkey < 0 || partkey >= part_capacity) {
				continue;
			}
			const auto group_slot = part_group_slot[static_cast<size_t>(partkey)];
			if (group_slot == 0) {
				continue;
			}
			const auto suppkey = suppkeys[suppkey_index];
			if (IsExcludedSupplier(suppkey)) {
				continue;
			}
			RecordGroupSupplier(group_slot - 1, suppkey);
		}
	}

	Q16Parameters params;
	Q16ProfileCounters *profile;
	Q16RuntimeConfig config;
	TwoPartLikeMatcher complaint_matcher;
	PrefixLikeMatcher type_prefix_matcher;
	StringInterner brand_intern;
	StringInterner type_intern;
	std::array<uint8_t, kMaxPartSize + 1> allowed_sizes {};
	std::vector<uint8_t> excluded_suppliers;
	std::vector<int32_t> part_group_slot;
	std::vector<PartGroup> groups;
	std::unordered_map<CompactGroupKey, int32_t, CompactGroupKeyHash> group_index;
	std::vector<GroupSuppPair> group_supplier_pairs;
	bool has_qualifying_parts = false;
	std::vector<Q16OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ16Kernel(const Q16Parameters &params) {
	return duckdb::make_uniq<Q16Kernel>(params);
}

duckdb::unique_ptr<KernelInstance> CreateQ16ProfilingKernel(const Q16Parameters &params, Q16ProfileCounters *profile) {
	return duckdb::make_uniq<Q16Kernel>(params, profile);
}

} // namespace accelerator_codegen
