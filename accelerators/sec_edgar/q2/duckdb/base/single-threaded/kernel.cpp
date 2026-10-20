#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "sec_edgar/q2/duckdb/base/query.hpp"
#include "sec_edgar/q2/duckdb/base/dictionary_slot_cache.hpp"
#include "sec_edgar/q2/duckdb/base/fused.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr duckdb::idx_t kResultLimit = 100;

constexpr duckdb::idx_t kSubAdshScanIndex = 0;
constexpr duckdb::idx_t kSubNameScanIndex = 1;

constexpr duckdb::idx_t kNumAdshScanIndex = 0;
constexpr duckdb::idx_t kNumTagScanIndex = 1;
constexpr duckdb::idx_t kNumValueScanIndex = 2;

struct AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static AdshKey From(const duckdb::string_t &adsh) {
		AdshKey key;
		key.size = static_cast<uint32_t>(adsh.GetSize());
		const auto *data = adsh.GetData();
		if (data == nullptr || key.size == 0) {
			return key;
		}
		if (key.size > 0) {
			std::memcpy(&key.w0, data, std::min<duckdb::idx_t>(key.size, 8));
		}
		if (key.size > 8) {
			std::memcpy(&key.w1, data + 8, std::min<duckdb::idx_t>(key.size - 8, 8));
		}
		if (key.size > 16) {
			std::memcpy(&key.w2, data + 16, std::min<duckdb::idx_t>(key.size - 16, 8));
		}
		return key;
	}

	static AdshKey FromStored(const std::string &adsh) {
		return From(duckdb::string_t(adsh));
	}

	bool operator==(const AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct AdshKeyHash {
	size_t operator()(const AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

// Tags run to ~100 bytes: the first 24 in an AdshKey, the rest (if any) kept
// whole so distinct tags sharing a 24-byte prefix stay distinct groups.
struct TagKey {
	AdshKey head;
	std::string tail;

	static TagKey From(const duckdb::string_t &tag) {
		TagKey key;
		key.head = AdshKey::From(tag);
		if (tag.GetSize() > 24) {
			key.tail.assign(tag.GetData() + 24, tag.GetSize() - 24);
		}
		return key;
	}

	static TagKey FromStored(const std::string &tag) {
		return From(duckdb::string_t(tag));
	}

	bool operator==(const TagKey &other) const {
		return head == other.head && tail == other.tail;
	}
};

struct TagKeyHash {
	size_t operator()(const TagKey &key) const noexcept {
		const size_t head = AdshKeyHash {}(key.head);
		return key.tail.empty() ? head : head ^ (std::hash<std::string> {}(key.tail) * 0x9e3779b97f4a7c15ULL);
	}
};

struct GroupKey {
	AdshKey adsh;
	TagKey tag;

	static GroupKey From(const duckdb::string_t &adsh, const duckdb::string_t &tag) {
		return {AdshKey::From(adsh), TagKey::From(tag)};
	}

	static GroupKey FromStored(const std::string &adsh, const std::string &tag) {
		return {AdshKey::FromStored(adsh), TagKey::FromStored(tag)};
	}

	bool operator==(const GroupKey &other) const {
		return adsh == other.adsh && tag == other.tag;
	}
};

struct GroupKeyHash {
	size_t operator()(const GroupKey &key) const noexcept {
		return AdshKeyHash {}(key.adsh) ^ (TagKeyHash {}(key.tag) << 1);
	}
};

struct CandidateRow {
	std::string name;
	std::string tag;
	double value = 0.0;
};

struct NumRowBuffer {
	std::string adsh;
	std::string tag;
	double value = 0.0;
};

std::string ToStoredString(const duckdb::string_t &value) {
	const auto size = value.GetSize();
	if (size == 0) {
		return std::string();
	}
	const auto *data = value.GetData();
	if (data == nullptr) {
		return std::string(size, '\0');
	}
	return std::string(data, size);
}

struct AdshTagGroup {
	double max_value = 0.0;
	bool has_max = false;
	std::vector<CandidateRow> ties;
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

enum class ConsumableKind {
	Flat,
	Dict,
	Constant,
};

struct ConsumableStringColumn {
	ConsumableKind kind = ConsumableKind::Flat;
	const duckdb::string_t *data = nullptr;
	const duckdb::SelectionVector *sel = nullptr;
	duckdb::ValidityMask validity {};
	bool all_valid = false;
};

struct ConsumableDoubleColumn {
	ConsumableKind kind = ConsumableKind::Flat;
	const double *data = nullptr;
	const duckdb::SelectionVector *sel = nullptr;
	duckdb::ValidityMask validity {};
	bool all_valid = false;
};

bool BindConsumableString(duckdb::Vector &vector, ConsumableStringColumn &out) {
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		out.kind = ConsumableKind::Flat;
		out.data = duckdb::FlatVector::GetData<duckdb::string_t>(vector);
		out.validity = duckdb::FlatVector::Validity(vector);
		out.all_valid = out.validity.AllValid();
		return true;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			return false;
		}
		out.kind = ConsumableKind::Dict;
		out.data = duckdb::FlatVector::GetData<duckdb::string_t>(child);
		out.validity = duckdb::FlatVector::Validity(child);
		out.all_valid = out.validity.AllValid();
		out.sel = &duckdb::DictionaryVector::SelVector(vector);
		return true;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		out.kind = ConsumableKind::Constant;
		out.data = duckdb::ConstantVector::GetData<duckdb::string_t>(vector);
		out.all_valid = !duckdb::ConstantVector::IsNull(vector);
		return true;
	default:
		return false;
	}
}

bool BindConsumableDouble(duckdb::Vector &vector, ConsumableDoubleColumn &out) {
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		out.kind = ConsumableKind::Flat;
		out.data = duckdb::FlatVector::GetData<double>(vector);
		out.validity = duckdb::FlatVector::Validity(vector);
		out.all_valid = out.validity.AllValid();
		return true;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			return false;
		}
		out.kind = ConsumableKind::Dict;
		out.data = duckdb::FlatVector::GetData<double>(child);
		out.validity = duckdb::FlatVector::Validity(child);
		out.all_valid = out.validity.AllValid();
		out.sel = &duckdb::DictionaryVector::SelVector(vector);
		return true;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		out.kind = ConsumableKind::Constant;
		out.data = duckdb::ConstantVector::GetData<double>(vector);
		out.all_valid = !duckdb::ConstantVector::IsNull(vector);
		return true;
	default:
		return false;
	}
}

duckdb::idx_t ConsumableRowIndex(const ConsumableStringColumn &column, duckdb::idx_t base) {
	if (column.kind == ConsumableKind::Constant) {
		return 0;
	}
	if (column.kind == ConsumableKind::Dict) {
		return column.sel->get_index(base);
	}
	return base;
}

duckdb::idx_t ConsumableRowIndex(const ConsumableDoubleColumn &column, duckdb::idx_t base) {
	if (column.kind == ConsumableKind::Constant) {
		return 0;
	}
	if (column.kind == ConsumableKind::Dict) {
		return column.sel->get_index(base);
	}
	return base;
}

bool ConsumableEntryValid(const ConsumableStringColumn &column, duckdb::idx_t index) {
	return column.all_valid || column.validity.RowIsValidUnsafe(index);
}

bool ConsumableEntryValid(const ConsumableDoubleColumn &column, duckdb::idx_t index) {
	return column.all_valid || column.validity.RowIsValidUnsafe(index);
}

int ConsumableChunkPath(const ConsumableStringColumn &adsh, const ConsumableStringColumn &tag,
                        const ConsumableDoubleColumn &value) {
	if (adsh.kind == ConsumableKind::Flat && tag.kind == ConsumableKind::Flat && value.kind == ConsumableKind::Flat &&
	    adsh.all_valid && tag.all_valid && value.all_valid) {
		return 0;
	}
	return 1;
}

int ConsumableChunkPath(const ConsumableStringColumn &adsh, const ConsumableStringColumn &name) {
	if (adsh.kind == ConsumableKind::Flat && name.kind == ConsumableKind::Flat && adsh.all_valid && name.all_valid) {
		return 0;
	}
	return 1;
}

void TrackChunkPath(SecEdgarQ2ProfileCounters *profile, duckdb::idx_t rows, int path) {
	if (!profile) {
		return;
	}
	profile->pre_rows += rows;
	if (path == 0) {
		profile->flat_chunks++;
	} else if (path == 1) {
		profile->dict_chunks++;
	} else {
		profile->generic_chunks++;
		profile->generic_fallback_rows += rows;
	}
}

template <typename RowFn>
void ProcessSubConsumableChunk(duckdb::idx_t count, ConsumableStringColumn &adsh_col, ConsumableStringColumn &name_col,
                               SecEdgarQ2ProfileCounters *profile, RowFn &&row_fn) {
	TrackChunkPath(profile, count, ConsumableChunkPath(adsh_col, name_col));
	if (adsh_col.kind == ConsumableKind::Flat && name_col.kind == ConsumableKind::Flat && adsh_col.all_valid &&
	    name_col.all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			row_fn(adsh_col.data[row], name_col.data[row]);
		}
		return;
	}
	const bool shared_sel = adsh_col.kind == ConsumableKind::Dict && name_col.kind == ConsumableKind::Dict &&
	                        adsh_col.sel == name_col.sel && adsh_col.all_valid && name_col.all_valid;
	if (shared_sel) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto index = adsh_col.sel->get_index(row);
			row_fn(adsh_col.data[index], name_col.data[index]);
		}
		return;
	}
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto adsh_index = ConsumableRowIndex(adsh_col, row);
		const auto name_index = ConsumableRowIndex(name_col, row);
		if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(name_col, name_index)) {
			continue;
		}
		row_fn(adsh_col.data[adsh_index], name_col.data[name_index]);
	}
}

template <typename RowFn>
void ProcessNumConsumableChunk(duckdb::idx_t count, ConsumableStringColumn &adsh_col, ConsumableStringColumn &tag_col,
                               ConsumableDoubleColumn &value_col, SecEdgarQ2ProfileCounters *profile,
                               RowFn &&row_fn) {
	TrackChunkPath(profile, count, ConsumableChunkPath(adsh_col, tag_col, value_col));
	if (adsh_col.kind == ConsumableKind::Flat && tag_col.kind == ConsumableKind::Flat &&
	    value_col.kind == ConsumableKind::Flat && adsh_col.all_valid && tag_col.all_valid && value_col.all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			row_fn(adsh_col.data[row], tag_col.data[row], value_col.data[row]);
		}
		return;
	}
	const bool shared_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
	                        value_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
	                        adsh_col.sel == value_col.sel && adsh_col.all_valid && tag_col.all_valid &&
	                        value_col.all_valid;
	if (shared_sel) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto index = adsh_col.sel->get_index(row);
			row_fn(adsh_col.data[index], tag_col.data[index], value_col.data[index]);
		}
		return;
	}
	const bool shared_string_dict = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
	                                  adsh_col.sel == tag_col.sel && adsh_col.all_valid && tag_col.all_valid;
	if (shared_string_dict && value_col.kind == ConsumableKind::Flat && value_col.all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto index = adsh_col.sel->get_index(row);
			row_fn(adsh_col.data[index], tag_col.data[index], value_col.data[row]);
		}
		return;
	}
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto adsh_index = ConsumableRowIndex(adsh_col, row);
		const auto tag_index = ConsumableRowIndex(tag_col, row);
		const auto value_index = ConsumableRowIndex(value_col, row);
		if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(tag_col, tag_index) ||
		    !ConsumableEntryValid(value_col, value_index)) {
			continue;
		}
		row_fn(adsh_col.data[adsh_index], tag_col.data[tag_index], value_col.data[value_index]);
	}
}

} // namespace

struct SecEdgarQ2Kernel::State {
	explicit State(duckdb::idx_t row_estimate) {
		const auto reserve_groups = row_estimate > 0 ? row_estimate / 8 : 1024;
		adsh_tag_groups.reserve(reserve_groups);
		fy2022_adsh_to_name.reserve(row_estimate > 0 ? row_estimate / 100 : 4096);
		num_buffer.reserve(row_estimate > 0 ? row_estimate / 4 : 65536);
		adsh_dict_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
	}

	std::unordered_map<AdshKey, std::string, AdshKeyHash> fy2022_adsh_to_name;
	std::unordered_map<GroupKey, AdshTagGroup, GroupKeyHash> adsh_tag_groups;
	// Per adsh dictionary entry: its fy2022 name, or Absent when it has none.
	DictionarySlotCache<const std::string *> adsh_dict_cache;
	std::vector<CandidateRow> candidates;
	std::vector<CandidateRow> ordered_output;
	std::vector<NumRowBuffer> num_buffer;
	bool fused_scans_done = false;
	bool sub_consumed = false;
	bool num_consumed = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;

	void MaybeAddTie(AdshTagGroup &group, const std::string &name, const duckdb::string_t &tag, double value,
	                 SecEdgarQ2ProfileCounters *profile_counters) {
		group.ties.push_back({name, ToStoredString(tag), value});
		if (profile_counters) {
			profile_counters->qualifying_rows++;
		}
	}

	void ProcessNumRowWithName(const std::string &name, const duckdb::string_t &adsh, const duckdb::string_t &tag,
	                           double value, SecEdgarQ2ProfileCounters *profile_counters) {
		const auto group_key = GroupKey::From(adsh, tag);
		auto it = adsh_tag_groups.find(group_key);
		if (it == adsh_tag_groups.end()) {
			AdshTagGroup group;
			group.has_max = true;
			group.max_value = value;
			MaybeAddTie(group, name, tag, value, profile_counters);
			adsh_tag_groups.emplace(group_key, std::move(group));
			if (profile_counters) {
				profile_counters->distinct_inserts++;
			}
			return;
		}
		auto &group = it->second;
		if (!group.has_max || value > group.max_value) {
			group.has_max = true;
			group.max_value = value;
			group.ties.clear();
			MaybeAddTie(group, name, tag, value, profile_counters);
			return;
		}
		if (value == group.max_value) {
			MaybeAddTie(group, name, tag, value, profile_counters);
		}
	}

	const std::string *LookupFy2022Flat(const duckdb::string_t &adsh) {
		const auto adsh_key = AdshKey::From(adsh);
		const auto name_it = fy2022_adsh_to_name.find(adsh_key);
		if (name_it == fy2022_adsh_to_name.end()) {
			return nullptr;
		}
		return &name_it->second;
	}

	const std::string *LookupFy2022Dict(duckdb::idx_t dict_idx, const duckdb::string_t &adsh) {
		using Slot = DictionarySlotCache<const std::string *>::Slot;
		const auto slot = adsh_dict_cache.State(dict_idx);
		if (slot == Slot::Absent) {
			return nullptr;
		}
		if (slot == Slot::Present) {
			return adsh_dict_cache.Value(dict_idx);
		}
		const auto *name = LookupFy2022Flat(adsh);
		if (name == nullptr) {
			adsh_dict_cache.StoreAbsent(dict_idx);
		} else {
			adsh_dict_cache.StorePresent(dict_idx, name);
		}
		return name;
	}

	// adsh_vec: the vector adsh_col is bound to.
	void ProcessNumConsumableFiltered(duckdb::idx_t count, const duckdb::Vector &adsh_vec,
	                                  ConsumableStringColumn &adsh_col, ConsumableStringColumn &tag_col,
	                                  ConsumableDoubleColumn &value_col, SecEdgarQ2ProfileCounters *profile_counters) {
		adsh_dict_cache.BeginChunk(adsh_vec);
		TrackChunkPath(profile_counters, count, ConsumableChunkPath(adsh_col, tag_col, value_col));
		if (adsh_col.kind == ConsumableKind::Flat && tag_col.kind == ConsumableKind::Flat &&
		    value_col.kind == ConsumableKind::Flat && adsh_col.all_valid && tag_col.all_valid && value_col.all_valid) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto *name = LookupFy2022Flat(adsh_col.data[row]);
				if (name == nullptr) {
					continue;
				}
				ProcessNumRowWithName(*name, adsh_col.data[row], tag_col.data[row], value_col.data[row],
				                      profile_counters);
			}
			return;
		}
		const bool shared_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
		                        value_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
		                        adsh_col.sel == value_col.sel && adsh_col.all_valid && tag_col.all_valid &&
		                        value_col.all_valid;
		if (shared_sel) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto index = adsh_col.sel->get_index(row);
				const auto *name = LookupFy2022Dict(index, adsh_col.data[index]);
				if (name == nullptr) {
					continue;
				}
				ProcessNumRowWithName(*name, adsh_col.data[index], tag_col.data[index], value_col.data[index],
				                      profile_counters);
			}
			return;
		}
		const bool shared_string_dict = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
		                                  adsh_col.sel == tag_col.sel && adsh_col.all_valid && tag_col.all_valid;
		if (shared_string_dict && value_col.kind == ConsumableKind::Flat && value_col.all_valid) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto index = adsh_col.sel->get_index(row);
				const auto *name = LookupFy2022Dict(index, adsh_col.data[index]);
				if (name == nullptr) {
					continue;
				}
				ProcessNumRowWithName(*name, adsh_col.data[index], tag_col.data[index], value_col.data[row],
				                      profile_counters);
			}
			return;
		}
		// Most rows' adsh is not a fy2022 filing: settle that before touching
		// the tag / value columns.
		const auto finish_row = [&](duckdb::idx_t row, const std::string &name, const duckdb::string_t &adsh) {
			const auto tag_index = ConsumableRowIndex(tag_col, row);
			const auto value_index = ConsumableRowIndex(value_col, row);
			if (!ConsumableEntryValid(tag_col, tag_index) || !ConsumableEntryValid(value_col, value_index)) {
				return;
			}
			ProcessNumRowWithName(name, adsh, tag_col.data[tag_index], value_col.data[value_index], profile_counters);
		};
		if (adsh_col.kind == ConsumableKind::Dict) {
			const auto &adsh_sel = *adsh_col.sel;
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto adsh_index = adsh_sel.get_index(row);
				if (!adsh_col.all_valid && !adsh_col.validity.RowIsValidUnsafe(adsh_index)) {
					continue;
				}
				const auto *name = LookupFy2022Dict(adsh_index, adsh_col.data[adsh_index]);
				if (name != nullptr) {
					finish_row(row, *name, adsh_col.data[adsh_index]);
				}
			}
			return;
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = ConsumableRowIndex(adsh_col, row);
			if (!ConsumableEntryValid(adsh_col, adsh_index)) {
				continue;
			}
			const auto *name = LookupFy2022Flat(adsh_col.data[adsh_index]);
			if (name != nullptr) {
				finish_row(row, *name, adsh_col.data[adsh_index]);
			}
		}
	}

	void ProcessNumRow(const duckdb::string_t &adsh, const duckdb::string_t &tag, double value,
	                   SecEdgarQ2ProfileCounters *profile_counters) {
		const auto *name = LookupFy2022Flat(adsh);
		if (name == nullptr) {
			return;
		}
		ProcessNumRowWithName(*name, adsh, tag, value, profile_counters);
	}

	void MaybeAddTieFromStored(AdshTagGroup &group, const std::string &name, const std::string &tag, double value,
	                           SecEdgarQ2ProfileCounters *profile_counters) {
		group.ties.push_back({name, tag, value});
		if (profile_counters) {
			profile_counters->qualifying_rows++;
		}
	}

	void ProcessNumRow(const std::string &adsh, const std::string &tag, double value,
	                   SecEdgarQ2ProfileCounters *profile_counters) {
		const auto name_it = fy2022_adsh_to_name.find(AdshKey::FromStored(adsh));
		if (name_it == fy2022_adsh_to_name.end()) {
			return;
		}
		const auto &name = name_it->second;

		const auto group_key = GroupKey::FromStored(adsh, tag);
		auto it = adsh_tag_groups.find(group_key);
		if (it == adsh_tag_groups.end()) {
			AdshTagGroup group;
			group.has_max = true;
			group.max_value = value;
			MaybeAddTieFromStored(group, name, tag, value, profile_counters);
			adsh_tag_groups.emplace(group_key, std::move(group));
			if (profile_counters) {
				profile_counters->distinct_inserts++;
			}
			return;
		}
		auto &group = it->second;
		if (!group.has_max || value > group.max_value) {
			group.has_max = true;
			group.max_value = value;
			group.ties.clear();
			MaybeAddTieFromStored(group, name, tag, value, profile_counters);
			return;
		}
		if (value == group.max_value) {
			MaybeAddTieFromStored(group, name, tag, value, profile_counters);
		}
	}

	void InsertSubRow(const duckdb::string_t &adsh, const duckdb::string_t &name) {
		fy2022_adsh_to_name.try_emplace(AdshKey::From(adsh), ToStoredString(name));
	}

	void FlushGroupTies() {
		for (auto &entry : adsh_tag_groups) {
			for (auto &tie : entry.second.ties) {
				candidates.push_back(std::move(tie));
			}
			entry.second.ties.clear();
		}
	}

	void ProcessBufferedNumRows(SecEdgarQ2ProfileCounters *profile_counters) {
		for (const auto &row : num_buffer) {
			ProcessNumRow(row.adsh, row.tag, row.value, profile_counters);
		}
		num_buffer.clear();
	}
};

SecEdgarQ2Kernel::SecEdgarQ2Kernel(SecEdgarQ2Parameters params_p, SecEdgarQ2ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ2Kernel::~SecEdgarQ2Kernel() = default;

bool SecEdgarQ2Kernel::InputsConsumed() const {
	return state->fused_scans_done || (state->sub_consumed && state->num_consumed);
}

void SecEdgarQ2Kernel::MarkInputsConsumed() {
	state->fused_scans_done = true;
}

void SecEdgarQ2Kernel::ConsumeSubChunk(duckdb::DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	state->sub_consumed = true;
	auto &adsh_vec = input.data[SecEdgarQ2SubChunkLayout::ADSH_INDEX];
	auto &name_vec = input.data[SecEdgarQ2SubChunkLayout::NAME_INDEX];

	ConsumableStringColumn adsh_col;
	ConsumableStringColumn name_col;
	if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(name_vec, name_col)) {
		ProcessSubConsumableChunk(input.size(), adsh_col, name_col, profile,
		                          [this](const duckdb::string_t &adsh, const duckdb::string_t &name) {
			                          state->InsertSubRow(adsh, name);
		                          });
		return;
	}

	TrackChunkPath(profile, input.size(), 2);
	duckdb::UnifiedVectorFormat adsh_format;
	duckdb::UnifiedVectorFormat name_format;
	adsh_vec.ToUnifiedFormat(input.size(), adsh_format);
	name_vec.ToUnifiedFormat(input.size(), name_format);
	auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
	auto name_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto adsh_index = adsh_format.sel->get_index(row);
		const auto name_index = name_format.sel->get_index(row);
		if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
			continue;
		}
		if (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) {
			continue;
		}
		state->InsertSubRow(adsh_values[adsh_index], name_values[name_index]);
	}
}

void SecEdgarQ2Kernel::ConsumeNumChunk(duckdb::DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	state->num_consumed = true;
	auto &adsh_vec = input.data[SecEdgarQ2NumChunkLayout::ADSH_INDEX];
	auto &tag_vec = input.data[SecEdgarQ2NumChunkLayout::TAG_INDEX];
	auto &value_vec = input.data[SecEdgarQ2NumChunkLayout::VALUE_INDEX];

	const auto consume_start = Clock::now();

	ConsumableStringColumn adsh_col;
	ConsumableStringColumn tag_col;
	ConsumableDoubleColumn value_col;
	if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
	    BindConsumableDouble(value_vec, value_col)) {
		ProcessNumConsumableChunk(
		    input.size(), adsh_col, tag_col, value_col, profile,
		    [this](const duckdb::string_t &adsh, const duckdb::string_t &tag, double value) {
			    state->num_buffer.push_back({ToStoredString(adsh), ToStoredString(tag), value});
		    });
	} else {
		TrackChunkPath(profile, input.size(), 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat value_format;
		adsh_vec.ToUnifiedFormat(input.size(), adsh_format);
		tag_vec.ToUnifiedFormat(input.size(), tag_format);
		value_vec.ToUnifiedFormat(input.size(), value_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto value_values = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
		for (duckdb::idx_t row = 0; row < input.size(); row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto value_index = value_format.sel->get_index(row);
			if (!value_format.validity.AllValid() && !value_format.validity.RowIsValidUnsafe(value_index)) {
				continue;
			}
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!tag_format.validity.AllValid() && !tag_format.validity.RowIsValidUnsafe(tag_index)) {
				continue;
			}
			state->num_buffer.push_back(
			    {ToStoredString(adsh_values[adsh_index]), ToStoredString(tag_values[tag_index]), value_values[value_index]});
		}
	}

	if (profile) {
		profile->pre_consume_us +=
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count();
	}
}

void SecEdgarQ2Kernel::ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) {
	if (input_id == static_cast<duckdb::idx_t>(SecEdgarQ2InputId::SUB)) {
		ConsumeSubChunk(input);
		return;
	}
	if (input_id == static_cast<duckdb::idx_t>(SecEdgarQ2InputId::NUM)) {
		ConsumeNumChunk(input);
		return;
	}
	throw duckdb::InternalException("SEC EDGAR Q2 received unexpected input id %llu", input_id);
}

void SecEdgarQ2Kernel::RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto consumer = [this](duckdb::TransactionData transaction, duckdb::CollectionScanState &scan_state,
	                       duckdb::RowGroup &row_group, duckdb::SelectionVector &sel,
	                       duckdb::idx_t count) -> duckdb::FilteredScanConsumerResult {
		(void)scan_state;
		if (count == 0) {
			return duckdb::FilteredScanConsumerResult::CONTINUE;
		}
		duckdb::Vector adsh_vec(duckdb::LogicalType::VARCHAR);
		duckdb::Vector name_vec(duckdb::LogicalType::VARCHAR);
		row_group.SelectProjectedColumn(transaction, scan_state, kSubAdshScanIndex, sel, count, adsh_vec,
		                              duckdb::UpdateScanType::STANDARD);
		row_group.SelectProjectedColumn(transaction, scan_state, kSubNameScanIndex, sel, count, name_vec,
		                              duckdb::UpdateScanType::STANDARD);
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn name_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(name_vec, name_col)) {
			ProcessSubConsumableChunk(count, adsh_col, name_col, profile,
			                          [this](const duckdb::string_t &adsh, const duckdb::string_t &name) {
				                          state->InsertSubRow(adsh, name);
			                          });
		} else {
			TrackChunkPath(profile, count, 2);
			duckdb::UnifiedVectorFormat adsh_format;
			duckdb::UnifiedVectorFormat name_format;
			adsh_vec.ToUnifiedFormat(count, adsh_format);
			name_vec.ToUnifiedFormat(count, name_format);
			auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
			auto name_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto adsh_index = adsh_format.sel->get_index(row);
				const auto name_index = name_format.sel->get_index(row);
				if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
					continue;
				}
				if (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) {
					continue;
				}
				state->InsertSubRow(adsh_values[adsh_index], name_values[name_index]);
			}
		}
		return duckdb::FilteredScanConsumerResult::CONTINUE;
	};
	if (!RunNativeFilteredScanConsumer(context, bind_data, consumer)) {
		throw duckdb::InterruptException();
	}
	if (profile) {
		profile->sub_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ2Kernel::RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		auto &adsh_vec = chunk.data[SecEdgarQ2NumChunkLayout::ADSH_INDEX];
		auto &tag_vec = chunk.data[SecEdgarQ2NumChunkLayout::TAG_INDEX];
		auto &value_vec = chunk.data[SecEdgarQ2NumChunkLayout::VALUE_INDEX];

		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableDoubleColumn value_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		    BindConsumableDouble(value_vec, value_col)) {
			state->ProcessNumConsumableFiltered(chunk.size(), adsh_vec, adsh_col, tag_col, value_col, profile);
			continue;
		}

		TrackChunkPath(profile, chunk.size(), 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat value_format;
		adsh_vec.ToUnifiedFormat(chunk.size(), adsh_format);
		tag_vec.ToUnifiedFormat(chunk.size(), tag_format);
		value_vec.ToUnifiedFormat(chunk.size(), value_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto value_values = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
		for (duckdb::idx_t row = 0; row < chunk.size(); row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto value_index = value_format.sel->get_index(row);
			if (!value_format.validity.AllValid() && !value_format.validity.RowIsValidUnsafe(value_index)) {
				continue;
			}
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!tag_format.validity.AllValid() && !tag_format.validity.RowIsValidUnsafe(tag_index)) {
				continue;
			}
			const auto &adsh = adsh_values[adsh_index];
			const auto *name = state->LookupFy2022Flat(adsh);
			if (name == nullptr) {
				continue;
			}
			state->ProcessNumRowWithName(*name, adsh, tag_values[tag_index], value_values[value_index], profile);
		}
	}
	if (profile) {
		profile->num_max_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
		profile->num_collect_us = 0;
		profile->active_groups = state->adsh_tag_groups.size();
	}
}

void SecEdgarQ2Kernel::FinalizeCandidates() {
	if (state->finalized) {
		return;
	}

	if (!state->fused_scans_done && !state->num_buffer.empty()) {
		state->ProcessBufferedNumRows(profile);
	}

	state->FlushGroupTies();

	state->ordered_output = std::move(state->candidates);
	std::sort(state->ordered_output.begin(), state->ordered_output.end(),
	          [](const CandidateRow &lhs, const CandidateRow &rhs) {
		          if (lhs.value != rhs.value) {
			          return lhs.value > rhs.value;
		          }
		          if (lhs.name != rhs.name) {
			          return lhs.name < rhs.name;
		          }
		          return lhs.tag < rhs.tag;
	          });
	if (state->ordered_output.size() > kResultLimit) {
		state->ordered_output.resize(kResultLimit);
	}

	if (profile) {
		profile->candidate_rows = state->ordered_output.size();
	}
	state->finalized = true;
}

void SecEdgarQ2Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeCandidates();
	if (profile) {
		profile->finalize_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

bool SecEdgarQ2Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_output.size()) {
		return false;
	}
	const auto remaining = state->ordered_output.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_output[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(result.name));
		output.SetValue(1, row, duckdb::Value(result.tag));
		output.SetValue(2, row, duckdb::Value::DOUBLE(result.value));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2Kernel(const SecEdgarQ2Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ2Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2ProfilingKernel(const SecEdgarQ2Parameters &params,
                                                                    SecEdgarQ2ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ2Kernel>(params, &counters);
}

SecEdgarQ2FusedKernel::SecEdgarQ2FusedKernel(const SecEdgarQ2Parameters &params, SecEdgarQ2ProfileCounters *profile_p)
    : SecEdgarQ2Kernel(params, profile_p) {
}

void SecEdgarQ2FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q2 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ2FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind) {
	if (InputsConsumed()) {
		return;
	}

	const auto scan_start = Clock::now();
	const auto sub_bind = BindNativeScan(context, CreateSecEdgarQ2SubScanSpec());
	RunSubScan(context, *sub_bind);
	state->adsh_dict_cache.Reserve(state->fy2022_adsh_to_name.size() * 2);
	RunNumFusedPass(context, num_bind);
	MarkInputsConsumed();

	if (profile) {
		profile->pre_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void RunSecEdgarQ2FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ2FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, num_bind);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2FusedKernel(const SecEdgarQ2Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ2FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ2FusedProfilingKernel(const SecEdgarQ2Parameters &params,
                                                                          SecEdgarQ2ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ2FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
