#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_kernel.hpp"
#include "tpch/q16/duckdb/base/query.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"

namespace accelerator_codegen {
namespace {

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr char kComplaintPattern[] = "%Customer%Complaints%";
constexpr int64_t kDefaultSupplierCapacity = 10001;
constexpr int64_t kDefaultPartCapacity = 200001;
constexpr int32_t kMaxPartSize = 50;

bool Q16ProfileEnabled() {
	if (const char *env = std::getenv("ACCEL_PARALLEL_PROFILE"); env && env[0] == '1') {
		return true;
	}
	return false;
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

struct Q16ParallelProfile {
	int64_t total_ns = 0;
	int64_t supplier_build_ns = 0;
	int64_t part_build_ns = 0;
	int64_t partsupp_scan_ns = 0;
	int64_t pair_merge_ns = 0;
	int64_t distinct_ns = 0;
	int64_t output_ns = 0;
	size_t worker_count = 0;
	size_t suppliers_scanned = 0;
	size_t supplier_comments_inspected = 0;
	size_t supplier_comment_bytes = 0;
	size_t suppliers_excluded = 0;
	size_t parts_scanned = 0;
	size_t parts_rejected_size = 0;
	size_t parts_rejected_brand = 0;
	size_t parts_rejected_type = 0;
	size_t parts_qualifying = 0;
	size_t group_count = 0;
	size_t partsupp_rows_scanned = 0;
	size_t partsupp_rejected_part = 0;
	size_t partsupp_rejected_supplier = 0;
	size_t partsupp_qualifying = 0;
	size_t raw_pairs = 0;
	size_t distinct_pairs = 0;
	size_t supplier_flat_rows = 0;
	size_t supplier_dict_rows = 0;
	size_t supplier_fallback_rows = 0;
	size_t part_flat_rows = 0;
	size_t part_dict_rows = 0;
	size_t part_fallback_rows = 0;
	size_t partsupp_flat_rows = 0;
	size_t partsupp_dict_rows = 0;
	size_t partsupp_fallback_rows = 0;
	int64_t max_worker_ns = 0;
	int64_t avg_worker_ns = 0;
};

struct Q16WorkerPartsuppProfile {
	size_t rows = 0;
	int64_t elapsed_ns = 0;
};

void PrintQ16ParallelProfile(const Q16ParallelProfile &profile) {
	const auto pct = [&](int64_t phase_ns) {
		return profile.total_ns > 0 ? (100.0 * static_cast<double>(phase_ns) / static_cast<double>(profile.total_ns))
		                            : 0.0;
	};
	std::cerr << "q16_parallel_profile total_ns=" << profile.total_ns
	          << " supplier_build_ns=" << profile.supplier_build_ns << " (" << pct(profile.supplier_build_ns) << "%)"
	          << " part_build_ns=" << profile.part_build_ns << " (" << pct(profile.part_build_ns) << "%)"
	          << " partsupp_scan_ns=" << profile.partsupp_scan_ns << " (" << pct(profile.partsupp_scan_ns) << "%)"
	          << " pair_merge_ns=" << profile.pair_merge_ns << " (" << pct(profile.pair_merge_ns) << "%)"
	          << " distinct_ns=" << profile.distinct_ns << " (" << pct(profile.distinct_ns) << "%)"
	          << " output_ns=" << profile.output_ns << " (" << pct(profile.output_ns) << "%)"
	          << " worker_count=" << profile.worker_count << " suppliers_scanned=" << profile.suppliers_scanned
	          << " supplier_comments_inspected=" << profile.supplier_comments_inspected
	          << " supplier_comment_bytes=" << profile.supplier_comment_bytes
	          << " suppliers_excluded=" << profile.suppliers_excluded << " parts_scanned=" << profile.parts_scanned
	          << " parts_rejected_size=" << profile.parts_rejected_size
	          << " parts_rejected_brand=" << profile.parts_rejected_brand
	          << " parts_rejected_type=" << profile.parts_rejected_type << " parts_qualifying=" << profile.parts_qualifying
	          << " group_count=" << profile.group_count << " partsupp_rows_scanned=" << profile.partsupp_rows_scanned
	          << " partsupp_rejected_part=" << profile.partsupp_rejected_part
	          << " partsupp_rejected_supplier=" << profile.partsupp_rejected_supplier
	          << " partsupp_qualifying=" << profile.partsupp_qualifying << " raw_pairs=" << profile.raw_pairs
	          << " distinct_pairs=" << profile.distinct_pairs << " supplier_flat_rows=" << profile.supplier_flat_rows
	          << " supplier_dict_rows=" << profile.supplier_dict_rows
	          << " supplier_fallback_rows=" << profile.supplier_fallback_rows << " part_flat_rows=" << profile.part_flat_rows
	          << " part_dict_rows=" << profile.part_dict_rows << " part_fallback_rows=" << profile.part_fallback_rows
	          << " partsupp_flat_rows=" << profile.partsupp_flat_rows
	          << " partsupp_dict_rows=" << profile.partsupp_dict_rows
	          << " partsupp_fallback_rows=" << profile.partsupp_fallback_rows
	          << " max_worker_ns=" << profile.max_worker_ns << " avg_worker_ns=" << profile.avg_worker_ns << '\n';
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

struct Q16FrozenState {
	const Q16Parameters &params;
	TwoPartLikeMatcher complaint_matcher;
	PrefixLikeMatcher type_prefix_matcher;
	StringInterner brand_intern;
	StringInterner type_intern;
	std::array<uint8_t, kMaxPartSize + 1> allowed_sizes {};
	std::vector<uint8_t> excluded_suppliers;
	std::vector<int32_t> part_group_slot;
	std::vector<PartGroup> groups;
	std::unordered_map<CompactGroupKey, int32_t, CompactGroupKeyHash> group_index;
	bool has_qualifying_parts = false;

	explicit Q16FrozenState(const Q16Parameters &params_p)
	    : params(params_p), complaint_matcher(kComplaintPattern), type_prefix_matcher(params_p.type_pattern) {
		allowed_sizes.fill(0);
		for (const auto size : params.sizes) {
			if (size >= 1 && size <= kMaxPartSize) {
				allowed_sizes[static_cast<size_t>(size)] = 1;
			}
		}
		excluded_suppliers.assign(static_cast<size_t>(kDefaultSupplierCapacity), 0);
		part_group_slot.assign(static_cast<size_t>(kDefaultPartCapacity), 0);
	}

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
		if (complaint_matcher.enabled) {
			return complaint_matcher.Matches(comment);
		}
		return SqlLike(comment, kComplaintPattern);
	}

	bool TypeMatchesExcludedPrefix(const duckdb::string_t &type_value) const {
		if (type_prefix_matcher.enabled) {
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

	void QualifyPart(int64_t partkey, const duckdb::string_t &brand_value, const duckdb::string_t &type_value,
	                 int32_t size) {
		const auto group_id =
		    GetOrCreateGroupId(brand_intern.Intern(brand_value), type_intern.Intern(type_value), size);
		EnsurePartCapacity(partkey);
		part_group_slot[static_cast<size_t>(partkey)] = group_id + 1;
		has_qualifying_parts = true;
	}
};

void BuildSupplierState(duckdb::ClientContext &context, const duckdb::FunctionData &supplier_bind,
                        Q16FrozenState &state, Q16ParallelProfile *profile) {
	auto source = CreateNativeChunkSource(context, supplier_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (profile) {
			profile->suppliers_scanned += chunk.size();
		}
		auto &suppkey_vector = chunk.data[Q16SupplierChunkLayout::SUPPKEY_INDEX];
		auto &comment_vector = chunk.data[Q16SupplierChunkLayout::COMMENT_INDEX];

		if (IsFlatAllValid(suppkey_vector) && IsFlatAllValid(comment_vector)) {
			if (profile) {
				profile->supplier_flat_rows += chunk.size();
			}
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(comment_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				if (profile) {
					profile->supplier_comments_inspected += 1;
					profile->supplier_comment_bytes += comments[row].GetSize();
				}
				if (!state.CommentMatchesComplaint(comments[row])) {
					continue;
				}
				state.MarkExcludedSupplier(suppkeys[row]);
				if (profile) {
					profile->suppliers_excluded += 1;
				}
			}
			continue;
		}

		if (IsDictionaryOverFlat(suppkey_vector) && IsDictionaryOverFlat(comment_vector)) {
			if (profile) {
				profile->supplier_dict_rows += chunk.size();
			}
			auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
			auto &comment_sel = duckdb::DictionaryVector::SelVector(comment_vector);
			auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
			auto comments = duckdb::FlatVector::GetData<duckdb::string_t>(duckdb::DictionaryVector::Child(comment_vector));
			auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
			auto &comment_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(comment_vector));
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto suppkey_index = suppkey_sel.get_index(row);
				const auto comment_index = comment_sel.get_index(row);
				if (!suppkey_validity.RowIsValid(suppkey_index) || !comment_validity.RowIsValid(comment_index)) {
					if (profile) {
						profile->supplier_fallback_rows += 1;
					}
					continue;
				}
				if (profile) {
					profile->supplier_comments_inspected += 1;
					profile->supplier_comment_bytes += comments[comment_index].GetSize();
				}
				if (!state.CommentMatchesComplaint(comments[comment_index])) {
					continue;
				}
				state.MarkExcludedSupplier(suppkeys[suppkey_index]);
				if (profile) {
					profile->suppliers_excluded += 1;
				}
			}
			continue;
		}

		if (profile) {
			profile->supplier_fallback_rows += chunk.size();
		}
		duckdb::UnifiedVectorFormat suppkey_format;
		duckdb::UnifiedVectorFormat comment_format;
		suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
		comment_vector.ToUnifiedFormat(chunk.size(), comment_format);
		auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);
		auto comments = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(comment_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto suppkey_index = suppkey_format.sel->get_index(row);
			const auto comment_index = comment_format.sel->get_index(row);
			if ((!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index)) ||
			    (!comment_format.validity.AllValid() && !comment_format.validity.RowIsValidUnsafe(comment_index))) {
				continue;
			}
			if (profile) {
				profile->supplier_comments_inspected += 1;
				profile->supplier_comment_bytes += comments[comment_index].GetSize();
			}
			if (!state.CommentMatchesComplaint(comments[comment_index])) {
				continue;
			}
			state.MarkExcludedSupplier(suppkeys[suppkey_index]);
			if (profile) {
				profile->suppliers_excluded += 1;
			}
		}
	}
}

void BuildPartState(duckdb::ClientContext &context, const duckdb::FunctionData &part_bind, Q16FrozenState &state,
                    Q16ParallelProfile *profile) {
	auto source = CreateNativeChunkSource(context, part_bind);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (profile) {
			profile->parts_scanned += chunk.size();
		}
		auto &partkey_vector = chunk.data[Q16PartChunkLayout::PARTKEY_INDEX];
		auto &brand_vector = chunk.data[Q16PartChunkLayout::BRAND_INDEX];
		auto &type_vector = chunk.data[Q16PartChunkLayout::TYPE_INDEX];
		auto &size_vector = chunk.data[Q16PartChunkLayout::SIZE_INDEX];

		if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(brand_vector) && IsFlatAllValid(type_vector) &&
		    IsFlatAllValid(size_vector)) {
			if (profile) {
				profile->part_flat_rows += chunk.size();
			}
			auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
			auto brands = duckdb::FlatVector::GetData<duckdb::string_t>(brand_vector);
			auto types = duckdb::FlatVector::GetData<duckdb::string_t>(type_vector);
			auto sizes = duckdb::FlatVector::GetData<int32_t>(size_vector);
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				if (!state.SizeAllowed(sizes[row])) {
					if (profile) {
						profile->parts_rejected_size += 1;
					}
					continue;
				}
				if (StringEquals(brands[row], state.params.brand)) {
					if (profile) {
						profile->parts_rejected_brand += 1;
					}
					continue;
				}
				if (state.TypeMatchesExcludedPrefix(types[row])) {
					if (profile) {
						profile->parts_rejected_type += 1;
					}
					continue;
				}
				state.QualifyPart(partkeys[row], brands[row], types[row], sizes[row]);
				if (profile) {
					profile->parts_qualifying += 1;
				}
			}
			continue;
		}

		if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(brand_vector) &&
		    IsDictionaryOverFlat(type_vector) && IsDictionaryOverFlat(size_vector)) {
			if (profile) {
				profile->part_dict_rows += chunk.size();
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
			for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
				const auto partkey_index = partkey_sel.get_index(row);
				const auto brand_index = brand_sel.get_index(row);
				const auto type_index = type_sel.get_index(row);
				const auto size_index = size_sel.get_index(row);
				if (!partkey_validity.RowIsValid(partkey_index) || !brand_validity.RowIsValid(brand_index) ||
				    !type_validity.RowIsValid(type_index) || !size_validity.RowIsValid(size_index)) {
					if (profile) {
						profile->part_fallback_rows += 1;
					}
					continue;
				}
				const auto size = sizes[size_index];
				if (!state.SizeAllowed(size)) {
					if (profile) {
						profile->parts_rejected_size += 1;
					}
					continue;
				}
				if (StringEquals(brands[brand_index], state.params.brand)) {
					if (profile) {
						profile->parts_rejected_brand += 1;
					}
					continue;
				}
				if (state.TypeMatchesExcludedPrefix(types[type_index])) {
					if (profile) {
						profile->parts_rejected_type += 1;
					}
					continue;
				}
				state.QualifyPart(partkeys[partkey_index], brands[brand_index], types[type_index], size);
				if (profile) {
					profile->parts_qualifying += 1;
				}
			}
			continue;
		}

		if (profile) {
			profile->part_fallback_rows += chunk.size();
		}
		duckdb::UnifiedVectorFormat partkey_format;
		duckdb::UnifiedVectorFormat brand_format;
		duckdb::UnifiedVectorFormat type_format;
		duckdb::UnifiedVectorFormat size_format;
		partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
		brand_vector.ToUnifiedFormat(chunk.size(), brand_format);
		type_vector.ToUnifiedFormat(chunk.size(), type_format);
		size_vector.ToUnifiedFormat(chunk.size(), size_format);
		auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
		auto brands = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(brand_format);
		auto types = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(type_format);
		auto sizes = duckdb::UnifiedVectorFormat::GetData<int32_t>(size_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_format.sel->get_index(row);
			const auto brand_index = brand_format.sel->get_index(row);
			const auto type_index = type_format.sel->get_index(row);
			const auto size_index = size_format.sel->get_index(row);
			if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
			    (!brand_format.validity.AllValid() && !brand_format.validity.RowIsValidUnsafe(brand_index)) ||
			    (!type_format.validity.AllValid() && !type_format.validity.RowIsValidUnsafe(type_index)) ||
			    (!size_format.validity.AllValid() && !size_format.validity.RowIsValidUnsafe(size_index))) {
				continue;
			}
			const auto size = sizes[size_index];
			if (!state.SizeAllowed(size)) {
				if (profile) {
					profile->parts_rejected_size += 1;
				}
				continue;
			}
			if (StringEquals(brands[brand_index], state.params.brand)) {
				if (profile) {
					profile->parts_rejected_brand += 1;
				}
				continue;
			}
			if (state.TypeMatchesExcludedPrefix(types[type_index])) {
				if (profile) {
					profile->parts_rejected_type += 1;
				}
				continue;
			}
			state.QualifyPart(partkeys[partkey_index], brands[brand_index], types[type_index], size);
			if (profile) {
				profile->parts_qualifying += 1;
			}
		}
	}
}

bool TryEmitPartsuppPair(int64_t partkey, int64_t suppkey, const Q16FrozenState &state, int64_t part_capacity,
                         std::vector<GroupSuppPair> &group_supplier_pairs, Q16ParallelProfile *profile) {
	if (partkey < 0 || partkey >= part_capacity) {
		if (profile) {
			profile->partsupp_rejected_part += 1;
		}
		return false;
	}
	const auto group_slot = state.part_group_slot[static_cast<size_t>(partkey)];
	if (group_slot == 0) {
		if (profile) {
			profile->partsupp_rejected_part += 1;
		}
		return false;
	}
	if (state.IsExcludedSupplier(suppkey)) {
		if (profile) {
			profile->partsupp_rejected_supplier += 1;
		}
		return false;
	}
	if (suppkey < 0 || suppkey > std::numeric_limits<int32_t>::max()) {
		return false;
	}
	group_supplier_pairs.push_back(GroupSuppPair {group_slot - 1, static_cast<int32_t>(suppkey)});
	if (profile) {
		profile->partsupp_qualifying += 1;
		profile->raw_pairs += 1;
	}
	return true;
}

void AccumulatePartsuppChunk(duckdb::DataChunk &chunk, const Q16FrozenState &state,
                             std::vector<GroupSuppPair> &group_supplier_pairs, Q16ParallelProfile *profile) {
	if (!state.has_qualifying_parts) {
		return;
	}
	if (profile) {
		profile->partsupp_rows_scanned += chunk.size();
	}
	const auto part_capacity = static_cast<int64_t>(state.part_group_slot.size());
	auto &partkey_vector = chunk.data[Q16PartsuppChunkLayout::PARTKEY_INDEX];
	auto &suppkey_vector = chunk.data[Q16PartsuppChunkLayout::SUPPKEY_INDEX];

	if (IsFlatAllValid(partkey_vector) && IsFlatAllValid(suppkey_vector)) {
		if (profile) {
			profile->partsupp_flat_rows += chunk.size();
		}
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(partkey_vector);
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(suppkey_vector);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			TryEmitPartsuppPair(partkeys[row], suppkeys[row], state, part_capacity, group_supplier_pairs, profile);
		}
		return;
	}

	if (IsDictionaryOverFlat(partkey_vector) && IsDictionaryOverFlat(suppkey_vector)) {
		if (profile) {
			profile->partsupp_dict_rows += chunk.size();
		}
		auto &partkey_sel = duckdb::DictionaryVector::SelVector(partkey_vector);
		auto &suppkey_sel = duckdb::DictionaryVector::SelVector(suppkey_vector);
		auto partkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(partkey_vector));
		auto suppkeys = duckdb::FlatVector::GetData<int64_t>(duckdb::DictionaryVector::Child(suppkey_vector));
		auto &partkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(partkey_vector));
		auto &suppkey_validity = duckdb::FlatVector::Validity(duckdb::DictionaryVector::Child(suppkey_vector));
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto partkey_index = partkey_sel.get_index(row);
			const auto suppkey_index = suppkey_sel.get_index(row);
			if (!partkey_validity.RowIsValid(partkey_index) || !suppkey_validity.RowIsValid(suppkey_index)) {
				if (profile) {
					profile->partsupp_fallback_rows += 1;
				}
				continue;
			}
			TryEmitPartsuppPair(partkeys[partkey_index], suppkeys[suppkey_index], state, part_capacity,
			                    group_supplier_pairs, profile);
		}
		return;
	}

	if (profile) {
		profile->partsupp_fallback_rows += chunk.size();
	}
	duckdb::UnifiedVectorFormat partkey_format;
	duckdb::UnifiedVectorFormat suppkey_format;
	partkey_vector.ToUnifiedFormat(chunk.size(), partkey_format);
	suppkey_vector.ToUnifiedFormat(chunk.size(), suppkey_format);
	auto partkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(partkey_format);
	auto suppkeys = duckdb::UnifiedVectorFormat::GetData<int64_t>(suppkey_format);

	for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
		const auto partkey_index = partkey_format.sel->get_index(row);
		const auto suppkey_index = suppkey_format.sel->get_index(row);
		if ((!partkey_format.validity.AllValid() && !partkey_format.validity.RowIsValidUnsafe(partkey_index)) ||
		    (!suppkey_format.validity.AllValid() && !suppkey_format.validity.RowIsValidUnsafe(suppkey_index))) {
			continue;
		}
		TryEmitPartsuppPair(partkeys[partkey_index], suppkeys[suppkey_index], state, part_capacity,
		                    group_supplier_pairs, profile);
	}
}

void FinalizeGroupCounts(const Q16FrozenState &state, std::vector<GroupSuppPair> &group_supplier_pairs,
                         std::vector<Q16OutputRow> &output_rows, Q16ParallelProfile *profile) {
	const auto group_count = state.groups.size();
	std::vector<int64_t> group_counts(group_count, 0);
	if (!group_supplier_pairs.empty()) {
		std::vector<size_t> group_sizes(group_count, 0);
		for (const auto &pair : group_supplier_pairs) {
			if (pair.group_id >= 0 && static_cast<size_t>(pair.group_id) < group_count) {
				group_sizes[static_cast<size_t>(pair.group_id)]++;
			}
		}

		std::vector<size_t> group_offsets(group_count + 1, 0);
		for (size_t group_id = 0; group_id < group_count; group_id++) {
			group_offsets[group_id + 1] = group_offsets[group_id] + group_sizes[group_id];
		}

		std::vector<int32_t> grouped_suppkeys(group_offsets[group_count]);
		std::vector<size_t> write_cursor = group_offsets;
		for (const auto &pair : group_supplier_pairs) {
			if (pair.group_id < 0 || static_cast<size_t>(pair.group_id) >= group_count) {
				continue;
			}
			const auto group_id = static_cast<size_t>(pair.group_id);
			grouped_suppkeys[write_cursor[group_id]++] = pair.suppkey;
		}
		group_supplier_pairs.clear();
		group_supplier_pairs.shrink_to_fit();

		size_t max_suppkey = 0;
		for (const auto suppkey : grouped_suppkeys) {
			if (suppkey >= 0) {
				max_suppkey = std::max(max_suppkey, static_cast<size_t>(suppkey));
			}
		}

		std::vector<uint32_t> seen_generation(max_suppkey + 1, 0);
		uint32_t generation = 0;
		size_t distinct_pairs = 0;
		for (size_t group_id = 0; group_id < group_count; group_id++) {
			generation++;
			const auto begin = group_offsets[group_id];
			const auto end = group_offsets[group_id + 1];
			int64_t distinct = 0;
			for (size_t index = begin; index < end; index++) {
				const auto suppkey = static_cast<size_t>(grouped_suppkeys[index]);
				if (seen_generation[suppkey] != generation) {
					seen_generation[suppkey] = generation;
					distinct++;
					distinct_pairs++;
				}
			}
			group_counts[group_id] = distinct;
		}
		if (profile) {
			profile->distinct_pairs = distinct_pairs;
		}
	}

	output_rows.clear();
	output_rows.reserve(state.groups.size());
	for (size_t group_id = 0; group_id < state.groups.size(); group_id++) {
		if (group_counts[group_id] == 0) {
			continue;
		}
		Q16OutputRow row;
		row.brand = state.groups[group_id].brand;
		row.type = state.groups[group_id].type;
		row.size = state.groups[group_id].size;
		row.supplier_cnt = group_counts[group_id];
		output_rows.push_back(std::move(row));
	}
	std::sort(output_rows.begin(), output_rows.end(), BetterOutputRow);
}

class Q16ParallelKernel final : public ParallelKernelInstance {
public:
	explicit Q16ParallelKernel(Q16Parameters params_p) : params(std::move(params_p)) {
	}

	void RunParallel(duckdb::ClientContext &context,
	                 const duckdb::vector<duckdb::unique_ptr<duckdb::FunctionData>> &bound_inputs) override {
		if (bound_inputs.size() != 3 || !bound_inputs[0] || !bound_inputs[1] || !bound_inputs[2]) {
			throw duckdb::InternalException("Q16 parallel expects bound supplier, part, and partsupp scans");
		}

		using clock = std::chrono::steady_clock;
		const bool profile_enabled = Q16ProfileEnabled();
		Q16ParallelProfile profile;
		const auto total_t0 = clock::now();

		Q16FrozenState frozen(params);
		const auto supplier_t0 = clock::now();
		BuildSupplierState(context, *bound_inputs[0], frozen, profile_enabled ? &profile : nullptr);
		profile.supplier_build_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - supplier_t0).count();

		const auto part_t0 = clock::now();
		BuildPartState(context, *bound_inputs[1], frozen, profile_enabled ? &profile : nullptr);
		profile.part_build_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - part_t0).count();
		profile.group_count = frozen.groups.size();

		const auto worker_count = ParallelWorkerCount(context);
		profile.worker_count = static_cast<size_t>(worker_count);
		std::vector<CacheLinePadded<std::vector<GroupSuppPair>>> workers(worker_count);
		std::vector<Q16WorkerPartsuppProfile> worker_profiles(profile_enabled ? static_cast<size_t>(worker_count) : 0);

		const auto partsupp_t0 = clock::now();
		RunParallelChunkScan(context, *bound_inputs[2], worker_count,
		                     [&](duckdb::idx_t worker_id, duckdb::DataChunk &chunk) {
			                     const auto worker_t0 = profile_enabled ? clock::now() : clock::time_point {};
			                     AccumulatePartsuppChunk(chunk, frozen, workers[worker_id].value,
			                                             profile_enabled && worker_count == 1 ? &profile : nullptr);
			                     if (profile_enabled) {
				                     auto &worker_profile = worker_profiles[worker_id];
				                     worker_profile.rows += chunk.size();
				                     worker_profile.elapsed_ns +=
				                         std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - worker_t0)
				                             .count();
			                     }
		                     });
		profile.partsupp_scan_ns =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - partsupp_t0).count();
		if (profile_enabled) {
			int64_t worker_ns_sum = 0;
			for (const auto &worker_profile : worker_profiles) {
				worker_ns_sum += worker_profile.elapsed_ns;
				profile.max_worker_ns = std::max(profile.max_worker_ns, worker_profile.elapsed_ns);
			}
			profile.avg_worker_ns =
			    worker_profiles.empty() ? 0 : worker_ns_sum / static_cast<int64_t>(worker_profiles.size());
			if (worker_count > 1) {
				size_t worker_pairs = 0;
				for (const auto &worker : workers) {
					worker_pairs += worker.value.size();
				}
				profile.raw_pairs = worker_pairs;
			}
		}

		std::vector<GroupSuppPair> merged;
		size_t total_pairs = 0;
		for (const auto &worker : workers) {
			total_pairs += worker.value.size();
		}
		const auto merge_t0 = clock::now();
		merged.reserve(total_pairs);
		for (auto &worker : workers) {
			merged.insert(merged.end(), std::make_move_iterator(worker.value.begin()),
			              std::make_move_iterator(worker.value.end()));
			worker.value.clear();
		}
		profile.pair_merge_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - merge_t0).count();
		if (profile_enabled) {
			profile.raw_pairs = merged.size();
		}

		const auto distinct_t0 = clock::now();
		FinalizeGroupCounts(frozen, merged, output_rows, profile_enabled ? &profile : nullptr);
		profile.distinct_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - distinct_t0).count();

		const auto output_t0 = clock::now();
		// output rows already sorted inside FinalizeGroupCounts
		profile.output_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - output_t0).count();
		profile.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - total_t0).count();
		if (profile_enabled) {
			PrintQ16ParallelProfile(profile);
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
	Q16Parameters params;
	std::vector<Q16OutputRow> output_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
};

} // namespace

duckdb::unique_ptr<KernelInstance> CreateQ16ParallelKernel(const Q16Parameters &params) {
	return duckdb::make_uniq<Q16ParallelKernel>(params);
}

} // namespace accelerator_codegen
