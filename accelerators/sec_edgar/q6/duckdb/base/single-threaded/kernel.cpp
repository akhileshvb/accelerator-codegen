#include "backends/duckdb/api/duckdb_scan_adapter.hpp"
#include "sec_edgar/q6/duckdb/base/query.hpp"
#include "sec_edgar/q6/duckdb/base/fused.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/filter/bloom_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr duckdb::idx_t kResultLimit = 200;
constexpr int32_t kTargetFy = 2023;

const duckdb::string_t &UsdUomConstant() {
	static const duckdb::string_t kUsd = duckdb::string_t("USD");
	return kUsd;
}

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
		std::memcpy(&key.w0, data, std::min<duckdb::idx_t>(key.size, 8));
		if (key.size > 8) {
			std::memcpy(&key.w1, data + 8, std::min<duckdb::idx_t>(key.size - 8, 8));
		}
		if (key.size > 16) {
			std::memcpy(&key.w2, data + 16, std::min<duckdb::idx_t>(key.size - 16, 8));
		}
		return key;
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

struct TagVersionKey {
	AdshKey tag;
	AdshKey version;

	static TagVersionKey From(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return {AdshKey::From(tag), AdshKey::From(version)};
	}

	bool operator==(const TagVersionKey &other) const {
		return tag == other.tag && version == other.version;
	}
};

struct TagVersionKeyHash {
	size_t operator()(const TagVersionKey &key) const noexcept {
		return AdshKeyHash {}(key.tag) ^ (AdshKeyHash {}(key.version) << 1);
	}
};

struct TagVersionTable {
	struct StoredPair {
		std::string tag;
		std::string version;
	};

	uint32_t Intern(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return InternKeys(AdshKey::From(tag), AdshKey::From(version), tag, version);
	}

	uint32_t InternKeys(const AdshKey &tag_key, const AdshKey &version_key, const duckdb::string_t &tag,
	                    const duckdb::string_t &version) {
		const TagVersionKey key {tag_key, version_key};
		const auto range = lookup_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			const auto &stored = storage_[it->second];
			if (StoredStringEquals(stored.tag, tag) && StoredStringEquals(stored.version, version)) {
				return it->second;
			}
		}
		const uint32_t id = static_cast<uint32_t>(storage_.size());
		lookup_.emplace(key, id);
		storage_.push_back(
		    StoredPair {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())});
		return id;
	}

	bool Lookup(const duckdb::string_t &tag, const duckdb::string_t &version, uint32_t &out_id) const {
		return LookupKeys(AdshKey::From(tag), AdshKey::From(version), tag, version, out_id);
	}

	bool LookupKeys(const AdshKey &tag_key, const AdshKey &version_key, const duckdb::string_t &tag,
	                const duckdb::string_t &version, uint32_t &out_id) const {
		const TagVersionKey key {tag_key, version_key};
		const auto range = lookup_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			const auto &stored = storage_[it->second];
			if (StoredStringEquals(stored.tag, tag) && StoredStringEquals(stored.version, version)) {
				out_id = it->second;
				return true;
			}
		}
		return false;
	}

	void PopulateAux(SecEdgarQ6TagVersionPartition &out) const {
		out.tag_versions.clear();
		out.tag_versions.reserve(storage_.size());
		for (const auto &stored : storage_) {
			out.tag_versions.emplace_back(stored.tag, stored.version);
		}
		out.tag_version_lookup.clear();
		out.tag_version_lookup.reserve(lookup_.size());
		for (const auto &entry : lookup_) {
			const auto &stored = storage_[entry.second];
			out.tag_version_lookup.emplace(
			    SecEdgarQ6TagVersionKey::FromStored(stored.tag, stored.version), entry.second);
		}
	}

private:
	static bool StoredStringEquals(const std::string &stored, const duckdb::string_t &value) {
		return stored.size() == value.GetSize() &&
		       (stored.empty() || std::memcmp(stored.data(), value.GetData(), value.GetSize()) == 0);
	}

	std::vector<StoredPair> storage_;
	std::unordered_multimap<TagVersionKey, uint32_t, TagVersionKeyHash> lookup_;
};

class AdshInternTable {
public:
	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			lookup_.reserve(estimate);
		}
	}

	uint32_t Intern(const duckdb::string_t &adsh) {
		const AdshKey key = AdshKey::From(adsh);
		const auto it = lookup_.find(key);
		if (it != lookup_.end()) {
			return it->second;
		}
		const uint32_t id = static_cast<uint32_t>(lookup_.size());
		lookup_.emplace(key, id);
		return id;
	}

	bool Lookup(const duckdb::string_t &adsh, uint32_t &out_id) const {
		return LookupKey(AdshKey::From(adsh), out_id);
	}

	bool LookupKey(const AdshKey &key, uint32_t &out_id) const {
		const auto it = lookup_.find(key);
		if (it == lookup_.end()) {
			return false;
		}
		out_id = it->second;
		return true;
	}

	template <typename Fn>
	void ForEach(Fn &&fn) const {
		for (const auto &entry : lookup_) {
			fn(entry.first, entry.second);
		}
	}

private:
	std::unordered_map<AdshKey, uint32_t, AdshKeyHash> lookup_;
};

class KeyInterner {
public:
	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			lookup_.reserve(estimate);
			storage_.reserve(estimate);
		}
	}

	uint32_t Intern(const duckdb::string_t &value) {
		return InternKeys(AdshKey::From(value), value);
	}

	uint32_t InternKeys(const AdshKey &key, const duckdb::string_t &value) {
		const auto range = lookup_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			const auto &stored = storage_[it->second];
			if (StoredStringEquals(stored, value)) {
				return it->second;
			}
		}
		const uint32_t id = static_cast<uint32_t>(storage_.size());
		lookup_.emplace(key, id);
		storage_.emplace_back(value.GetData(), value.GetSize());
		return id;
	}

	const std::string &Get(uint32_t id) const {
		return storage_[id];
	}

	const std::vector<std::string> &Storage() const {
		return storage_;
	}

private:
	static bool StoredStringEquals(const std::string &stored, const duckdb::string_t &value) {
		return stored.size() == value.GetSize() &&
		       (stored.empty() || std::memcmp(stored.data(), value.GetData(), value.GetSize()) == 0);
	}

	std::vector<std::string> storage_;
	std::unordered_multimap<AdshKey, uint32_t, AdshKeyHash> lookup_;
};

struct PreEntry {
	uint32_t tag_id = 0;
	uint32_t plabel_id = 0;
	uint32_t multiplicity = 0;
	uint32_t group_id = 0;
};

using PreEntriesForTriple = std::vector<PreEntry>;

struct ResolvedNumTriple {
	const PreEntriesForTriple *entries = nullptr;
	uint32_t single_group_id = 0;
	uint32_t single_multiplicity = 0;
	double single_weight = 0.0;
	bool single = false;
};

struct PreTvBucket {
	PreEntriesForTriple entries;
	ResolvedNumTriple resolved {};
	bool resolved_ready = false;
};

using PreByTagVersion = std::unordered_map<uint32_t, PreTvBucket>;

struct AdshTvKey {
	AdshKey adsh;
	uint32_t tv_id = 0;

	bool operator==(const AdshTvKey &other) const {
		return tv_id == other.tv_id && adsh == other.adsh;
	}
};

struct AdshTvKeyHash {
	size_t operator()(const AdshTvKey &key) const noexcept {
		return AdshKeyHash {}(key.adsh) ^ (static_cast<size_t>(key.tv_id) << 1);
	}
};

struct CompactGroupKey {
	uint32_t name_id = 0;
	uint32_t tag_id = 0;
	uint32_t plabel_id = 0;

	bool operator==(const CompactGroupKey &other) const {
		return name_id == other.name_id && tag_id == other.tag_id && plabel_id == other.plabel_id;
	}
};

struct CompactGroupKeyHash {
	size_t operator()(const CompactGroupKey &key) const noexcept {
		return (static_cast<size_t>(key.name_id) << 42) ^ (static_cast<size_t>(key.tag_id) << 21) ^ key.plabel_id;
	}
};

struct GroupAggregate {
	double sum = 0.0;
	int64_t count = 0;
	bool active = false;
};

struct GroupMeta {
	uint32_t name_id = 0;
	uint32_t tag_id = 0;
	uint32_t plabel_id = 0;
};

struct ResultRow {
	std::string name;
	std::string tag;
	std::string plabel;
	double total_value = 0.0;
	int64_t cnt = 0;
};

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

struct ConsumableInt32Column {
	ConsumableKind kind = ConsumableKind::Flat;
	const int32_t *data = nullptr;
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

bool BindConsumableInt32(duckdb::Vector &vector, ConsumableInt32Column &out) {
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		out.kind = ConsumableKind::Flat;
		out.data = duckdb::FlatVector::GetData<int32_t>(vector);
		out.validity = duckdb::FlatVector::Validity(vector);
		out.all_valid = out.validity.AllValid();
		return true;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			return false;
		}
		out.kind = ConsumableKind::Dict;
		out.data = duckdb::FlatVector::GetData<int32_t>(child);
		out.validity = duckdb::FlatVector::Validity(child);
		out.all_valid = out.validity.AllValid();
		out.sel = &duckdb::DictionaryVector::SelVector(vector);
		return true;
	}
	case duckdb::VectorType::CONSTANT_VECTOR:
		out.kind = ConsumableKind::Constant;
		out.data = duckdb::ConstantVector::GetData<int32_t>(vector);
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

duckdb::idx_t ConsumableRowIndex(const ConsumableInt32Column &column, duckdb::idx_t base) {
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

bool ConsumableEntryValid(const ConsumableInt32Column &column, duckdb::idx_t index) {
	return column.all_valid || column.validity.RowIsValidUnsafe(index);
}

bool IsUsdUom(const duckdb::string_t &uom) {
	return uom.GetSize() == 3 && uom.GetData() != nullptr && uom.GetData()[0] == 'U' && uom.GetData()[1] == 'S' &&
	       uom.GetData()[2] == 'D';
}

bool IsIsStmt(const duckdb::string_t &stmt) {
	return stmt.GetSize() == 2 && stmt.GetData() != nullptr && stmt.GetData()[0] == 'I' && stmt.GetData()[1] == 'S';
}

bool UomQualifies(const ConsumableStringColumn &uom_col, duckdb::idx_t row) {
	const auto index = ConsumableRowIndex(uom_col, row);
	if (!ConsumableEntryValid(uom_col, index)) {
		return false;
	}
	return IsUsdUom(uom_col.data[index]);
}

bool UomChunkAlwaysUsd(const ConsumableStringColumn &uom_col) {
	return uom_col.kind == ConsumableKind::Constant && uom_col.all_valid && IsUsdUom(uom_col.data[0]);
}

bool UomChunkNeverUsd(const ConsumableStringColumn &uom_col) {
	if (uom_col.kind == ConsumableKind::Constant) {
		return !uom_col.all_valid || !IsUsdUom(uom_col.data[0]);
	}
	return false;
}

void TrackChunkPath(SecEdgarQ6ProfileCounters *profile, duckdb::idx_t rows, int path) {
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

struct ProfileAggregateUs {
	SecEdgarQ6ProfileCounters *counters = nullptr;
	Clock::time_point start {};

	explicit ProfileAggregateUs(SecEdgarQ6ProfileCounters *profile_counters)
	    : counters(profile_counters), start(Clock::now()) {
	}

	~ProfileAggregateUs() {
		if (counters == nullptr) {
			return;
		}
		counters->num_aggregate_us +=
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
};

} // namespace

enum class NumPreQualifyStatus : uint8_t { Unknown = 0, Qualifies = 1, Rejects = 2 };

struct NumPreDictCache {
	std::vector<NumPreQualifyStatus> status;
	std::vector<uint32_t> adsh_ids;
	std::vector<const ResolvedNumTriple *> entries;

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			status.reserve(estimate);
			adsh_ids.reserve(estimate);
			entries.reserve(estimate);
		}
	}

	// Slots are indexes into this chunk's dictionary and are valid for this
	// chunk only.  (They used to be kept while the selection vector's address
	// repeated, but a later chunk's dictionary buffer can be allocated at a
	// freed one's address with other strings behind the same indexes.)
	void BeginChunk(const duckdb::SelectionVector *) {
		status.clear();
		adsh_ids.clear();
		entries.clear();
	}

	const ResolvedNumTriple *Lookup(duckdb::idx_t dict_idx, const duckdb::string_t &adsh,
	                                const duckdb::string_t &tag, const duckdb::string_t &version,
	                                const AdshInternTable &adsh_intern,
	                                const std::vector<PreByTagVersion> &pre_by_adsh,
	                                const TagVersionTable &tag_versions,
	                                const std::unordered_set<TagVersionKey, TagVersionKeyHash> &qualifying_tv_keys) {
		if (dict_idx >= status.size()) {
			status.resize(dict_idx + 1, NumPreQualifyStatus::Unknown);
			adsh_ids.resize(dict_idx + 1);
			entries.resize(dict_idx + 1);
		}
		auto &slot_status = status[dict_idx];
		if (slot_status != NumPreQualifyStatus::Unknown) {
			return slot_status == NumPreQualifyStatus::Qualifies ? entries[dict_idx] : nullptr;
		}
		const AdshKey tag_key = AdshKey::From(tag);
		const AdshKey version_key = AdshKey::From(version);
		if (!qualifying_tv_keys.empty()) {
			const TagVersionKey tv_key {tag_key, version_key};
			if (qualifying_tv_keys.find(tv_key) == qualifying_tv_keys.end()) {
				slot_status = NumPreQualifyStatus::Rejects;
				return nullptr;
			}
		}
		uint32_t adsh_id = 0;
		if (!adsh_intern.Lookup(adsh, adsh_id)) {
			slot_status = NumPreQualifyStatus::Rejects;
			return nullptr;
		}
		if (adsh_id >= pre_by_adsh.size() || pre_by_adsh[adsh_id].empty()) {
			slot_status = NumPreQualifyStatus::Rejects;
			return nullptr;
		}
		adsh_ids[dict_idx] = adsh_id;
		uint32_t tag_version_id = 0;
		if (!tag_versions.LookupKeys(tag_key, version_key, tag, version, tag_version_id)) {
			slot_status = NumPreQualifyStatus::Rejects;
			return nullptr;
		}
		if (adsh_id >= pre_by_adsh.size()) {
			slot_status = NumPreQualifyStatus::Rejects;
			return nullptr;
		}
		const auto &tv_map = pre_by_adsh[adsh_id];
		const auto bucket_it = tv_map.find(tag_version_id);
		if (bucket_it == tv_map.end() || !bucket_it->second.resolved_ready) {
			slot_status = NumPreQualifyStatus::Rejects;
			return nullptr;
		}
		slot_status = NumPreQualifyStatus::Qualifies;
		entries[dict_idx] = &bucket_it->second.resolved;
		return entries[dict_idx];
	}

	const ResolvedNumTriple *LookupVerified(duckdb::idx_t dict_idx) const {
		if (dict_idx >= status.size()) {
			return nullptr;
		}
		if (status[dict_idx] == NumPreQualifyStatus::Qualifies) {
			return entries[dict_idx];
		}
		return nullptr;
	}

	void Prewarm(const ConsumableStringColumn &adsh_col, const ConsumableStringColumn &tag_col,
	             const ConsumableStringColumn &version_col, duckdb::idx_t count,
	             const AdshInternTable &adsh_intern, const std::vector<PreByTagVersion> &pre_by_adsh,
	             const TagVersionTable &tag_versions,
	             const std::unordered_set<TagVersionKey, TagVersionKeyHash> &qualifying_tv_keys) {
		if (adsh_col.kind != ConsumableKind::Dict || tag_col.kind != ConsumableKind::Dict ||
		    version_col.kind != ConsumableKind::Dict) {
			return;
		}
		BeginChunk(adsh_col.sel);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto dict_idx = ConsumableRowIndex(adsh_col, row);
			if (dict_idx < status.size() && status[dict_idx] != NumPreQualifyStatus::Unknown) {
				continue;
			}
			const auto tag_index = ConsumableRowIndex(tag_col, row);
			const auto version_index = ConsumableRowIndex(version_col, row);
			Lookup(dict_idx, adsh_col.data[dict_idx], tag_col.data[tag_index], version_col.data[version_index],
			       adsh_intern, pre_by_adsh, tag_versions, qualifying_tv_keys);
		}
	}
};

struct SecEdgarQ6Kernel::State {
	explicit State(duckdb::idx_t row_estimate) {
		const auto reserve_groups = row_estimate > 0 ? row_estimate / 200 : 4096;
		const auto reserve_pre = row_estimate > 0 ? row_estimate / 20 : 65536;
		const auto reserve_sub = row_estimate > 0 ? row_estimate / 100 : 8192;
		const auto reserve_tv = row_estimate > 0 ? row_estimate / 40 : 16384;
		groups.reserve(reserve_groups);
		group_meta.reserve(reserve_groups);
		group_index.reserve(reserve_groups);
		adsh_intern.Reserve(reserve_sub);
		adsh_name_id.reserve(reserve_sub);
		pre_by_adsh.reserve(reserve_sub);
		num_pre_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
		qualifying_tv_keys.reserve(reserve_tv);
		tag_bloom_tag_set.reserve(reserve_tv / 4);
		tag_bloom_strings.reserve(reserve_tv / 4);
	}

	KeyInterner name_intern;
	KeyInterner tag_intern;
	KeyInterner plabel_intern;
	TagVersionTable tag_version_table;
	AdshInternTable adsh_intern;
	std::vector<uint32_t> adsh_name_id;
	std::vector<std::string> sub_adsh_strings;
	duckdb::BloomFilter adsh_bloom;
	duckdb::BloomFilter tag_bloom;
	duckdb::unique_ptr<duckdb::TableFilterSet> adsh_scan_filters;
	duckdb::unique_ptr<duckdb::TableFilterSet> num_scan_filters;
	bool adsh_bloom_ready = false;
	bool num_scan_filters_ready = false;
	std::unordered_set<TagVersionKey, TagVersionKeyHash> qualifying_tv_keys;
	std::unordered_set<std::string> tag_bloom_tag_set;
	std::vector<std::string> tag_bloom_strings;
	std::vector<PreByTagVersion> pre_by_adsh;
	std::unordered_map<CompactGroupKey, uint32_t, CompactGroupKeyHash> group_index;
	std::vector<GroupAggregate> groups;
	std::vector<GroupMeta> group_meta;
	std::vector<ResultRow> ordered_output;
	NumPreDictCache num_pre_cache;
	bool fused_scans_done = false;
	bool sub_fy_filter_pushed = false;
	bool pre_stmt_filter_pushed = false;
	bool num_uom_filter_pushed = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;

	void InsertSubRow(const duckdb::string_t &adsh, const duckdb::string_t &name, int32_t fy) {
		if (!sub_fy_filter_pushed && fy != kTargetFy) {
			return;
		}
		const auto adsh_id = adsh_intern.Intern(adsh);
		if (adsh_id < adsh_name_id.size()) {
			return;
		}
		if (adsh_name_id.size() <= adsh_id) {
			adsh_name_id.resize(adsh_id + 1);
			pre_by_adsh.resize(adsh_id + 1);
		}
		const auto name_id = name_intern.Intern(name);
		adsh_name_id[adsh_id] = name_id;
		sub_adsh_strings.emplace_back(adsh.GetData(), adsh.GetSize());
	}

	void BuildAdshBloomFilter(duckdb::ClientContext &context) {
		if (sub_adsh_strings.empty()) {
			return;
		}
		adsh_bloom.Initialize(context, sub_adsh_strings.size());
		duckdb::Vector adsh_values(duckdb::LogicalType::VARCHAR, sub_adsh_strings.size());
		adsh_values.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
		auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_values);
		for (duckdb::idx_t index = 0; index < sub_adsh_strings.size(); index++) {
			adsh_data[index] = duckdb::string_t(sub_adsh_strings[index]);
		}
		duckdb::Vector hashes(duckdb::LogicalType::HASH, sub_adsh_strings.size());
		duckdb::VectorOperations::Hash(adsh_values, hashes, sub_adsh_strings.size());
		adsh_bloom.InsertHashes(hashes, sub_adsh_strings.size());
		adsh_scan_filters = duckdb::make_uniq<duckdb::TableFilterSet>();
		adsh_scan_filters->PushFilter(duckdb::ColumnIndex(0),
		                              duckdb::make_uniq<duckdb::BFTableFilter>(adsh_bloom, true, "adsh",
		                                                                       duckdb::LogicalType::VARCHAR));
		adsh_bloom_ready = true;
	}

	const duckdb::TableFilterSet *AdshScanFilters() const {
		return adsh_bloom_ready ? adsh_scan_filters.get() : nullptr;
	}

	void BuildNumScanFilters(duckdb::ClientContext &context) {
		if (!adsh_bloom_ready) {
			BuildAdshBloomFilter(context);
		}
		if (!adsh_bloom_ready) {
			return;
		}
		num_scan_filters = duckdb::make_uniq<duckdb::TableFilterSet>();
		num_scan_filters->PushFilter(duckdb::ColumnIndex(0),
		                             duckdb::make_uniq<duckdb::BFTableFilter>(adsh_bloom, true, "adsh",
		                                                                      duckdb::LogicalType::VARCHAR));
		if (!tag_bloom_strings.empty()) {
			tag_bloom.Initialize(context, tag_bloom_strings.size());
			duckdb::Vector tag_values(duckdb::LogicalType::VARCHAR, tag_bloom_strings.size());
			tag_values.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
			auto *tag_data = duckdb::FlatVector::GetData<duckdb::string_t>(tag_values);
			for (duckdb::idx_t index = 0; index < tag_bloom_strings.size(); index++) {
				tag_data[index] = duckdb::string_t(tag_bloom_strings[index]);
			}
			duckdb::Vector hashes(duckdb::LogicalType::HASH, tag_bloom_strings.size());
			duckdb::VectorOperations::Hash(tag_values, hashes, tag_bloom_strings.size());
			tag_bloom.InsertHashes(hashes, tag_bloom_strings.size());
			num_scan_filters->PushFilter(duckdb::ColumnIndex(1),
			                             duckdb::make_uniq<duckdb::BFTableFilter>(tag_bloom, true, "tag",
			                                                                      duckdb::LogicalType::VARCHAR));
		}
		num_scan_filters_ready = true;
	}

	const duckdb::TableFilterSet *NumScanFilters() const {
		return num_scan_filters_ready ? num_scan_filters.get() : AdshScanFilters();
	}

	void InsertPreRow(const duckdb::string_t &adsh, const duckdb::string_t &tag, const duckdb::string_t &version,
	                  const duckdb::string_t &stmt, const duckdb::string_t &plabel) {
		if (!pre_stmt_filter_pushed && !IsIsStmt(stmt)) {
			return;
		}
		uint32_t adsh_id = 0;
		if (!adsh_intern.Lookup(adsh, adsh_id) || adsh_id >= adsh_name_id.size()) {
			return;
		}
		const auto name_id = adsh_name_id[adsh_id];
		const AdshKey tag_key = AdshKey::From(tag);
		const AdshKey version_key = AdshKey::From(version);
		const auto tag_id = tag_intern.InternKeys(tag_key, tag);
		const auto plabel_id = plabel_intern.InternKeys(AdshKey::From(plabel), plabel);
		const auto tv_id = tag_version_table.InternKeys(tag_key, version_key, tag, version);
		qualifying_tv_keys.insert(TagVersionKey {tag_key, version_key});
		const std::string tag_copy(tag.GetData(), tag.GetSize());
		if (tag_bloom_tag_set.insert(tag_copy).second) {
			tag_bloom_strings.push_back(tag_copy);
		}
		auto &bucket = pre_by_adsh[adsh_id][tv_id];
		for (auto &entry : bucket.entries) {
			if (entry.plabel_id == plabel_id) {
				entry.multiplicity++;
				return;
			}
		}
		bucket.entries.push_back(PreEntry {tag_id, plabel_id, 1, 0});
	}

	void FinalizePreLookup() {
		for (uint32_t adsh_id = 0; adsh_id < pre_by_adsh.size(); adsh_id++) {
			if (adsh_id >= adsh_name_id.size()) {
				continue;
			}
			const auto name_id = adsh_name_id[adsh_id];
			for (auto &tv_entry : pre_by_adsh[adsh_id]) {
				auto &bucket = tv_entry.second;
				if (bucket.entries.empty()) {
					continue;
				}
				for (auto &pre_entry : bucket.entries) {
					pre_entry.group_id = GetOrCreateGroupId(name_id, pre_entry.tag_id, pre_entry.plabel_id);
				}
				ResolvedNumTriple resolved;
				if (bucket.entries.size() == 1) {
					resolved.single = true;
					resolved.single_group_id = bucket.entries[0].group_id;
					resolved.single_multiplicity = bucket.entries[0].multiplicity;
					resolved.single_weight = static_cast<double>(bucket.entries[0].multiplicity);
				} else {
					resolved.entries = &bucket.entries;
				}
				bucket.resolved = resolved;
				bucket.resolved_ready = true;
			}
		}
	}

	void AccumulateResolved(const ResolvedNumTriple &resolved, double value,
	                        SecEdgarQ6ProfileCounters *profile_counters) {
		if (resolved.single) {
			auto &group = groups[resolved.single_group_id];
			if (group.count == 0) {
				if (profile_counters) {
					profile_counters->distinct_inserts++;
				}
			}
			group.sum += value * resolved.single_weight;
			group.count += static_cast<int64_t>(resolved.single_multiplicity);
			if (profile_counters) {
				profile_counters->qualifying_rows += resolved.single_multiplicity;
			}
			return;
		}
		AccumulatePreEntries(*resolved.entries, value, profile_counters);
	}

	void AccumulatePreEntries(const PreEntriesForTriple &pre_entries, double value,
	                          SecEdgarQ6ProfileCounters *profile_counters) {
		for (const auto &pre_entry : pre_entries) {
			auto &group = groups[pre_entry.group_id];
			if (group.count == 0) {
				if (profile_counters) {
					profile_counters->distinct_inserts++;
				}
			}
			const auto weight = static_cast<double>(pre_entry.multiplicity);
			group.sum += value * weight;
			group.count += static_cast<int64_t>(pre_entry.multiplicity);
			if (profile_counters) {
				profile_counters->qualifying_rows += pre_entry.multiplicity;
			}
		}
	}

	uint32_t GetOrCreateGroupId(uint32_t name_id, uint32_t tag_id, uint32_t plabel_id) {
		const CompactGroupKey key {name_id, tag_id, plabel_id};
		const auto it = group_index.find(key);
		if (it != group_index.end()) {
			return it->second;
		}
		const auto group_id = static_cast<uint32_t>(groups.size());
		groups.push_back(GroupAggregate {});
		group_meta.push_back(GroupMeta {name_id, tag_id, plabel_id});
		group_index.emplace(key, group_id);
		return group_id;
	}

	bool ProcessQualifyingRowIds(uint32_t adsh_id, uint32_t tv_id, double value,
	                             SecEdgarQ6ProfileCounters *profile_counters) {
		if (adsh_id >= pre_by_adsh.size()) {
			return false;
		}
		const auto &tv_map = pre_by_adsh[adsh_id];
		const auto bucket_it = tv_map.find(tv_id);
		if (bucket_it == tv_map.end() || !bucket_it->second.resolved_ready) {
			return false;
		}
		AccumulateResolved(bucket_it->second.resolved, value, profile_counters);
		return true;
	}

	bool ProcessQualifyingRow(const duckdb::string_t &adsh, const duckdb::string_t &tag,
	                          const duckdb::string_t &version, double value,
	                          SecEdgarQ6ProfileCounters *profile_counters) {
		uint32_t adsh_id = 0;
		if (!adsh_intern.Lookup(adsh, adsh_id)) {
			return false;
		}
		if (adsh_id >= pre_by_adsh.size() || pre_by_adsh[adsh_id].empty()) {
			return false;
		}
		const AdshKey tag_key = AdshKey::From(tag);
		const AdshKey version_key = AdshKey::From(version);
		uint32_t tv_id = 0;
		if (!tag_version_table.LookupKeys(tag_key, version_key, tag, version, tv_id)) {
			return false;
		}
		return ProcessQualifyingRowIds(adsh_id, tv_id, value, profile_counters);
	}

	bool ProcessQualifyingRowKeys(const AdshKey &adsh_key, const duckdb::string_t &adsh, const AdshKey &tag_key,
	                              const duckdb::string_t &tag, const AdshKey &version_key,
	                              const duckdb::string_t &version, double value,
	                              SecEdgarQ6ProfileCounters *profile_counters) {
		if (!qualifying_tv_keys.empty()) {
			const TagVersionKey tv_key {tag_key, version_key};
			if (qualifying_tv_keys.find(tv_key) == qualifying_tv_keys.end()) {
				return false;
			}
		}
		uint32_t adsh_id = 0;
		if (!adsh_intern.LookupKey(adsh_key, adsh_id)) {
			return false;
		}
		if (adsh_id >= pre_by_adsh.size() || pre_by_adsh[adsh_id].empty()) {
			return false;
		}
		uint32_t tv_id = 0;
		if (!tag_version_table.LookupKeys(tag_key, version_key, tag, version, tv_id)) {
			return false;
		}
		return ProcessQualifyingRowIds(adsh_id, tv_id, value, profile_counters);
	}

	void ProcessSubChunk(duckdb::DataChunk &input, SecEdgarQ6ProfileCounters *profile_counters) {
		if (input.size() == 0) {
			return;
		}
		auto &adsh_vec = input.data[0];
		auto &name_vec = input.data[1];
		auto &fy_vec = input.data[2];
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn name_col;
		ConsumableInt32Column fy_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(name_vec, name_col) &&
		    BindConsumableInt32(fy_vec, fy_col)) {
			TrackChunkPath(profile_counters, input.size(), 0);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto name_index = ConsumableRowIndex(name_col, row);
				const auto fy_index = ConsumableRowIndex(fy_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(name_col, name_index) ||
				    !ConsumableEntryValid(fy_col, fy_index)) {
					continue;
				}
				InsertSubRow(adsh_col.data[adsh_index], name_col.data[name_index], fy_col.data[fy_index]);
			}
			return;
		}
		TrackChunkPath(profile_counters, input.size(), 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat name_format;
		duckdb::UnifiedVectorFormat fy_format;
		const auto count = input.size();
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		name_vec.ToUnifiedFormat(count, name_format);
		fy_vec.ToUnifiedFormat(count, fy_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto name_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
		auto fy_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(fy_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto name_index = name_format.sel->get_index(row);
			const auto fy_index = fy_format.sel->get_index(row);
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) {
				continue;
			}
			if (!fy_format.validity.AllValid() && !fy_format.validity.RowIsValidUnsafe(fy_index)) {
				continue;
			}
			InsertSubRow(adsh_values[adsh_index], name_values[name_index], fy_values[fy_index]);
		}
	}

	void ProcessPreChunk(duckdb::DataChunk &input, SecEdgarQ6ProfileCounters *profile_counters) {
		if (input.size() == 0) {
			return;
		}
		auto &adsh_vec = input.data[0];
		auto &tag_vec = input.data[1];
		auto &version_vec = input.data[2];
		auto &stmt_vec = input.data[3];
		auto &plabel_vec = input.data[4];
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		ConsumableStringColumn stmt_col;
		ConsumableStringColumn plabel_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		    BindConsumableString(version_vec, version_col) && BindConsumableString(stmt_vec, stmt_col) &&
		    BindConsumableString(plabel_vec, plabel_col)) {
			TrackChunkPath(profile_counters, input.size(), 0);
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto tag_index = ConsumableRowIndex(tag_col, row);
				const auto version_index = ConsumableRowIndex(version_col, row);
				const auto stmt_index = ConsumableRowIndex(stmt_col, row);
				const auto plabel_index = ConsumableRowIndex(plabel_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(tag_col, tag_index) ||
				    !ConsumableEntryValid(version_col, version_index) || !ConsumableEntryValid(stmt_col, stmt_index) ||
				    !ConsumableEntryValid(plabel_col, plabel_index)) {
					continue;
				}
				InsertPreRow(adsh_col.data[adsh_index], tag_col.data[tag_index], version_col.data[version_index],
				             stmt_col.data[stmt_index], plabel_col.data[plabel_index]);
			}
			return;
		}
		TrackChunkPath(profile_counters, input.size(), 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat version_format;
		duckdb::UnifiedVectorFormat stmt_format;
		duckdb::UnifiedVectorFormat plabel_format;
		const auto count = input.size();
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		tag_vec.ToUnifiedFormat(count, tag_format);
		version_vec.ToUnifiedFormat(count, version_format);
		stmt_vec.ToUnifiedFormat(count, stmt_format);
		plabel_vec.ToUnifiedFormat(count, plabel_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
		auto stmt_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(stmt_format);
		auto plabel_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(plabel_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto version_index = version_format.sel->get_index(row);
			const auto stmt_index = stmt_format.sel->get_index(row);
			const auto plabel_index = plabel_format.sel->get_index(row);
			InsertPreRow(adsh_values[adsh_index], tag_values[tag_index], version_values[version_index],
			             stmt_values[stmt_index], plabel_values[plabel_index]);
		}
	}

	void ProcessNumChunkVectors(duckdb::idx_t count, duckdb::Vector &adsh_vec, duckdb::Vector &tag_vec,
	                            duckdb::Vector &version_vec, duckdb::Vector &value_vec, duckdb::Vector &uom_vec,
	                            SecEdgarQ6ProfileCounters *profile_counters) {
		ProfileAggregateUs aggregate_timer(profile_counters);
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		ConsumableDoubleColumn value_col;
		ConsumableStringColumn uom_col;
		if (!(BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		      BindConsumableString(version_vec, version_col) && BindConsumableDouble(value_vec, value_col))) {
			TrackChunkPath(profile_counters, count, 2);
			duckdb::UnifiedVectorFormat adsh_format;
			duckdb::UnifiedVectorFormat tag_format;
			duckdb::UnifiedVectorFormat version_format;
			duckdb::UnifiedVectorFormat value_format;
			duckdb::UnifiedVectorFormat uom_format;
			adsh_vec.ToUnifiedFormat(count, adsh_format);
			tag_vec.ToUnifiedFormat(count, tag_format);
			version_vec.ToUnifiedFormat(count, version_format);
			value_vec.ToUnifiedFormat(count, value_format);
			uom_vec.ToUnifiedFormat(count, uom_format);
			auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
			auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
			auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
			auto value_values = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
			auto uom_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(uom_format);
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto adsh_index = adsh_format.sel->get_index(row);
				const auto tag_index = tag_format.sel->get_index(row);
				const auto version_index = version_format.sel->get_index(row);
				const auto value_index = value_format.sel->get_index(row);
				const auto uom_index = uom_format.sel->get_index(row);
				if (!value_format.validity.AllValid() && !value_format.validity.RowIsValidUnsafe(value_index)) {
					continue;
				}
				if (!num_uom_filter_pushed) {
					if (!uom_format.validity.AllValid() && !uom_format.validity.RowIsValidUnsafe(uom_index)) {
						continue;
					}
					if (!IsUsdUom(uom_values[uom_index])) {
						continue;
					}
				}
				ProcessQualifyingRow(adsh_values[adsh_index], tag_values[tag_index], version_values[version_index],
				                     value_values[value_index], profile_counters);
			}
			return;
		}

		if (num_uom_filter_pushed) {
			uom_col.kind = ConsumableKind::Constant;
			uom_col.data = &UsdUomConstant();
			uom_col.all_valid = true;
		} else if (!BindConsumableString(uom_vec, uom_col)) {
			TrackChunkPath(profile_counters, count, 2);
			return;
		}

		const bool all_flat = adsh_col.kind == ConsumableKind::Flat && tag_col.kind == ConsumableKind::Flat &&
		                      version_col.kind == ConsumableKind::Flat && value_col.kind == ConsumableKind::Flat &&
		                      adsh_col.all_valid && tag_col.all_valid && version_col.all_valid && value_col.all_valid;
		TrackChunkPath(profile_counters, count, all_flat ? 0 : 1);
		if (UomChunkNeverUsd(uom_col)) {
			return;
		}
		const bool uom_always_usd = num_uom_filter_pushed || UomChunkAlwaysUsd(uom_col);
		const bool shared_dict_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
		                             version_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
		                             version_col.sel == tag_col.sel;
		if (shared_dict_sel) {
			num_pre_cache.Prewarm(adsh_col, tag_col, version_col, count, adsh_intern, pre_by_adsh, tag_version_table,
			                      qualifying_tv_keys);
			const bool value_dict_shared = value_col.kind == ConsumableKind::Dict && value_col.sel == adsh_col.sel;
			const auto &sel = *adsh_col.sel;
			const bool all_valid = adsh_col.all_valid && value_col.all_valid;
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto index = sel.get_index(row);
				const auto value_index = value_dict_shared ? index : ConsumableRowIndex(value_col, row);
				if (!all_valid &&
				    (!ConsumableEntryValid(adsh_col, index) || !ConsumableEntryValid(value_col, value_index))) {
					continue;
				}
				const auto *resolved = num_pre_cache.LookupVerified(index);
				if (resolved == nullptr) {
					continue;
				}
				AccumulateResolved(*resolved, value_col.data[value_index], profile_counters);
			}
			return;
		}
		if (all_flat) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto &adsh = adsh_col.data[row];
				const auto &tag = tag_col.data[row];
				const auto &version = version_col.data[row];
				ProcessQualifyingRowKeys(AdshKey::From(adsh), adsh, AdshKey::From(tag), tag, AdshKey::From(version),
				                         version, value_col.data[row], profile_counters);
			}
			return;
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!uom_always_usd && !UomQualifies(uom_col, row)) {
				continue;
			}
			const auto adsh_index = ConsumableRowIndex(adsh_col, row);
			const auto tag_index = ConsumableRowIndex(tag_col, row);
			const auto version_index = ConsumableRowIndex(version_col, row);
			const auto value_index = ConsumableRowIndex(value_col, row);
			if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(tag_col, tag_index) ||
			    !ConsumableEntryValid(version_col, version_index) || !ConsumableEntryValid(value_col, value_index)) {
				continue;
			}
			ProcessQualifyingRow(adsh_col.data[adsh_index], tag_col.data[tag_index], version_col.data[version_index],
			                     value_col.data[value_index], profile_counters);
		}
	}
};

SecEdgarQ6Kernel::SecEdgarQ6Kernel(SecEdgarQ6Parameters params_p, SecEdgarQ6ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ6Kernel::~SecEdgarQ6Kernel() = default;

bool SecEdgarQ6Kernel::InputsConsumed() const {
	return state->fused_scans_done;
}

void SecEdgarQ6Kernel::MarkInputsConsumed() {
	state->fused_scans_done = true;
}

void SecEdgarQ6Kernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q6 kernel: use fused pre-scan path");
}

void SecEdgarQ6Kernel::RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		state->ProcessSubChunk(source->Chunk(), profile);
	}
	if (profile) {
		profile->sub_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ6Kernel::RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data, state->AdshScanFilters());
	while (source->Next()) {
		state->ProcessPreChunk(source->Chunk(), profile);
	}
	if (profile) {
		profile->pre_table_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ6Kernel::RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data, state->NumScanFilters());
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		state->ProcessNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ6NumChunkLayout::ADSH_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::TAG_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::VERSION_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::VALUE_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::UOM_INDEX], profile);
	}
	if (profile) {
		profile->num_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
		duckdb::idx_t active = 0;
		for (const auto &group : state->groups) {
			if (group.count != 0) {
				active++;
			}
		}
		profile->active_groups = active;
	}
}

void SecEdgarQ6Kernel::FinalizeGroups() {
	if (state->finalized) {
		return;
	}
	state->ordered_output.clear();
	std::vector<uint32_t> active_group_ids;
	active_group_ids.reserve(state->groups.size());
	for (uint32_t group_id = 0; group_id < state->groups.size(); group_id++) {
		if (state->groups[group_id].count != 0) {
			active_group_ids.push_back(group_id);
		}
	}
	const auto compare_groups = [&](uint32_t lhs_id, uint32_t rhs_id) {
		const auto &lhs = state->groups[lhs_id];
		const auto &rhs = state->groups[rhs_id];
		if (lhs.sum != rhs.sum) {
			return lhs.sum > rhs.sum;
		}
		const auto &lhs_meta = state->group_meta[lhs_id];
		const auto &rhs_meta = state->group_meta[rhs_id];
		const auto &lhs_name = state->name_intern.Get(lhs_meta.name_id);
		const auto &rhs_name = state->name_intern.Get(rhs_meta.name_id);
		if (lhs_name != rhs_name) {
			return lhs_name < rhs_name;
		}
		const auto &lhs_tag = state->tag_intern.Get(lhs_meta.tag_id);
		const auto &rhs_tag = state->tag_intern.Get(rhs_meta.tag_id);
		if (lhs_tag != rhs_tag) {
			return lhs_tag < rhs_tag;
		}
		return state->plabel_intern.Get(lhs_meta.plabel_id) < state->plabel_intern.Get(rhs_meta.plabel_id);
	};
	const auto output_count = std::min<duckdb::idx_t>(active_group_ids.size(), kResultLimit);
	if (active_group_ids.size() > kResultLimit) {
		std::partial_sort(active_group_ids.begin(), active_group_ids.begin() + output_count, active_group_ids.end(),
		                  compare_groups);
		active_group_ids.resize(output_count);
	} else {
		std::sort(active_group_ids.begin(), active_group_ids.end(), compare_groups);
	}
	state->ordered_output.reserve(output_count);
	for (const auto group_id : active_group_ids) {
		const auto &group = state->groups[group_id];
		const auto &meta = state->group_meta[group_id];
		ResultRow row;
		row.name = state->name_intern.Get(meta.name_id);
		row.tag = state->tag_intern.Get(meta.tag_id);
		row.plabel = state->plabel_intern.Get(meta.plabel_id);
		row.total_value = group.sum;
		row.cnt = group.count;
		state->ordered_output.push_back(std::move(row));
	}
	if (profile) {
		profile->candidate_rows = state->ordered_output.size();
	}
	state->finalized = true;
}

void SecEdgarQ6Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeGroups();
	if (profile) {
		profile->finalize_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

SecEdgarQ6AdshKey ToAuxAdshKey(const AdshKey &key) {
	SecEdgarQ6AdshKey out;
	out.size = key.size;
	out.w0 = key.w0;
	out.w1 = key.w1;
	out.w2 = key.w2;
	return out;
}

SecEdgarQ6ResolvedPre ExportResolvedPre(const PreTvBucket &bucket) {
	SecEdgarQ6ResolvedPre out;
	if (bucket.resolved_ready && bucket.resolved.single) {
		out.single = true;
		out.single_group_id = bucket.resolved.single_group_id;
		out.single_multiplicity = bucket.resolved.single_multiplicity;
		return out;
	}
	out.entries.reserve(bucket.entries.size());
	for (const auto &entry : bucket.entries) {
		out.entries.push_back(
		    SecEdgarQ6ResolvedPreEntry {entry.tag_id, entry.plabel_id, entry.multiplicity, entry.group_id});
	}
	return out;
}

void SecEdgarQ6Kernel::ExportAuxPartitions(SecEdgarQ6Fy2023CohortPartition &cohort, SecEdgarQ6PreIsBridgePartition &pre_bridge,
                                           SecEdgarQ6TagVersionPartition &tag_versions,
                                           SecEdgarQ6GroupAggregatesPartition &aggregates, SecEdgarQ6OutputPartition &output) {
	if (!state->finalized) {
		FinalizeGroups();
	}

	cohort.adsh_to_name_id.clear();
	cohort.names = state->name_intern.Storage();
	state->adsh_intern.ForEach([&](const AdshKey &adsh_key, uint32_t adsh_id) {
		if (adsh_id < state->adsh_name_id.size()) {
			cohort.adsh_to_name_id.emplace(ToAuxAdshKey(adsh_key), state->adsh_name_id[adsh_id]);
		}
	});

	state->tag_version_table.PopulateAux(tag_versions);

	pre_bridge.pre_by_adsh.clear();
	state->adsh_intern.ForEach([&](const AdshKey &adsh_key, uint32_t adsh_id) {
		if (adsh_id >= state->pre_by_adsh.size() || state->pre_by_adsh[adsh_id].empty()) {
			return;
		}
		SecEdgarQ6PreEntriesByTagVersion entries;
		for (const auto &tv_entry : state->pre_by_adsh[adsh_id]) {
			entries.emplace(tv_entry.first, ExportResolvedPre(tv_entry.second));
		}
		pre_bridge.pre_by_adsh.emplace(ToAuxAdshKey(adsh_key), std::move(entries));
	});

	aggregates.group_meta.clear();
	aggregates.groups.clear();
	aggregates.tags = state->tag_intern.Storage();
	aggregates.plabels = state->plabel_intern.Storage();
	aggregates.group_meta.reserve(state->group_meta.size());
	aggregates.groups.reserve(state->groups.size());
	for (const auto &meta : state->group_meta) {
		aggregates.group_meta.push_back(SecEdgarQ6GroupMeta {meta.name_id, meta.tag_id, meta.plabel_id});
	}
	for (const auto &group : state->groups) {
		aggregates.groups.push_back(SecEdgarQ6GroupAggregate {group.sum, group.count, group.count != 0});
	}

	output.output_rows.clear();
	output.output_rows.reserve(state->ordered_output.size());
	for (const auto &row : state->ordered_output) {
		output.output_rows.push_back(
		    SecEdgarQ6OutputRow {row.name, row.tag, row.plabel, "IS", row.total_value, row.cnt});
	}
}

bool SecEdgarQ6Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_output.size()) {
		return false;
	}
	const auto remaining = state->ordered_output.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_output[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(result.name));
		output.SetValue(1, row, duckdb::Value("IS"));
		output.SetValue(2, row, duckdb::Value(result.tag));
		output.SetValue(3, row, duckdb::Value(result.plabel));
		output.SetValue(4, row, duckdb::Value::DOUBLE(result.total_value));
		output.SetValue(5, row, duckdb::Value::BIGINT(result.cnt));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6Kernel(const SecEdgarQ6Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ6Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6ProfilingKernel(const SecEdgarQ6Parameters &params,
                                                                    SecEdgarQ6ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ6Kernel>(params, &counters);
}

SecEdgarQ6FusedKernel::SecEdgarQ6FusedKernel(const SecEdgarQ6Parameters &params, SecEdgarQ6ProfileCounters *profile_p)
    : SecEdgarQ6Kernel(params, profile_p) {
}

void SecEdgarQ6Kernel::RunPostgresChunkPrescan(const PostgresChunkScanner &scan) {
	if (InputsConsumed()) {
		return;
	}
	state->sub_fy_filter_pushed = true;
	state->pre_stmt_filter_pushed = true;
	state->num_uom_filter_pushed = true;
	scan(CreateSecEdgarQ6SubScanSpec(),
	     [&](duckdb::DataChunk &chunk) { state->ProcessSubChunk(chunk, profile); });
	scan(CreateSecEdgarQ6PreScanSpec(),
	     [&](duckdb::DataChunk &chunk) { state->ProcessPreChunk(chunk, profile); });
	state->FinalizePreLookup();
	scan(CreateSecEdgarQ6NumScanSpec(), [&](duckdb::DataChunk &chunk) {
		if (chunk.size() == 0) {
			return;
		}
		state->ProcessNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ6NumChunkLayout::ADSH_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::TAG_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::VERSION_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::VALUE_INDEX],
		                              chunk.data[SecEdgarQ6NumChunkLayout::UOM_INDEX], profile);
	});
	MarkInputsConsumed();
}

void SecEdgarQ6FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q6 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ6FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind) {
	if (InputsConsumed()) {
		return;
	}
	const auto scan_start = Clock::now();
	const auto sub_bind = BindNativeScan(context, CreateSecEdgarQ6SubScanSpec());
	const auto pre_bind = BindNativeScan(context, CreateSecEdgarQ6PreScanSpec());
	state->sub_fy_filter_pushed = NativeScanHasFilters(*sub_bind);
	state->pre_stmt_filter_pushed = NativeScanHasFilters(*pre_bind);
	state->num_uom_filter_pushed = NativeScanHasFilters(num_bind);
	RunSubScan(context, *sub_bind);
	state->BuildAdshBloomFilter(context);
	RunPreScan(context, *pre_bind);
	state->FinalizePreLookup();
	state->BuildNumScanFilters(context);
	RunNumFusedPass(context, num_bind);
	MarkInputsConsumed();
	if (profile) {
		profile->pre_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void RunSecEdgarQ6FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ6FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, num_bind);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6FusedKernel(const SecEdgarQ6Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ6FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ6FusedProfilingKernel(const SecEdgarQ6Parameters &params,
                                                                          SecEdgarQ6ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ6FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
