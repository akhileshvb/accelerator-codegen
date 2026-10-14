#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "sec_edgar/q4/duckdb/base/query.hpp"
#include "sec_edgar/q4/duckdb/base/fused.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
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
constexpr duckdb::idx_t kResultLimit = 500;
constexpr int32_t kMinDistinctCik = 2;
constexpr int32_t kSicMin = 4000;
constexpr int32_t kSicMax = 4999;

const duckdb::string_t &EqStmtConstant() {
	static const duckdb::string_t kEq = duckdb::string_t("EQ");
	return kEq;
}

const duckdb::string_t &UsdUomConstant() {
	static const duckdb::string_t kUsd = duckdb::string_t("USD");
	return kUsd;
}

struct StringKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static StringKey From(const duckdb::string_t &value) {
		StringKey key;
		key.size = static_cast<uint32_t>(value.GetSize());
		const auto *data = value.GetData();
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

	bool operator==(const StringKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct StringKeyHash {
	size_t operator()(const StringKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

using AdshKey = StringKey;

struct TagVersionKey {
	std::string tag;
	std::string version;

	static TagVersionKey From(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())};
	}

	bool operator==(const TagVersionKey &other) const {
		return tag == other.tag && version == other.version;
	}
};

struct TagVersionKeyHash {
	size_t operator()(const TagVersionKey &key) const noexcept {
		return std::hash<std::string> {}(key.tag) ^ (std::hash<std::string> {}(key.version) << 1);
	}
};

class StringInterner;

struct TagVersionTable {
	uint32_t Intern(const duckdb::string_t &tag, const duckdb::string_t &version, uint32_t tlabel_id) {
		const auto key = TagVersionKey::From(tag, version);
		const auto it = lookup_.find(key);
		if (it != lookup_.end()) {
			return it->second;
		}
		const uint32_t id = static_cast<uint32_t>(tlabel_by_id_.size());
		lookup_.emplace(key, id);
		tlabel_by_id_.push_back(tlabel_id);
		return id;
	}

	bool Lookup(const duckdb::string_t &tag, const duckdb::string_t &version, uint32_t &out_id) const {
		const auto it = lookup_.find(TagVersionKey::From(tag, version));
		if (it == lookup_.end()) {
			return false;
		}
		out_id = it->second;
		return true;
	}

	bool LookupKey(const TagVersionKey &key, uint32_t &out_id) const {
		const auto it = lookup_.find(key);
		if (it == lookup_.end()) {
			return false;
		}
		out_id = it->second;
		return true;
	}

	uint32_t TlabelId(uint32_t tag_version_id) const {
		return tlabel_by_id_[tag_version_id];
	}

	void PopulateAuxPartition(const StringInterner &labels, SecEdgarQ4TagTlabelPartition &out) const;

private:
	std::unordered_map<TagVersionKey, uint32_t, TagVersionKeyHash> lookup_;
	std::vector<uint32_t> tlabel_by_id_;
};

struct ResolvedPreTriple {
	uint32_t group_id = 0;
	uint32_t cik_dense_id = 0;
	uint32_t multiplicity = 0;
};

using PreEntriesByTagVersion = std::unordered_map<uint32_t, ResolvedPreTriple>;
using PendingPreByTagVersion = std::unordered_map<TagVersionKey, uint32_t, TagVersionKeyHash>;

struct CompactGroupKey {
	int32_t sic = 0;
	uint32_t tlabel_id = 0;

	bool operator==(const CompactGroupKey &other) const {
		return sic == other.sic && tlabel_id == other.tlabel_id;
	}
};

struct CompactGroupKeyHash {
	size_t operator()(const CompactGroupKey &key) const noexcept {
		return (static_cast<size_t>(key.sic) << 32) ^ key.tlabel_id;
	}
};

class StringInterner {
public:
	uint32_t Intern(const duckdb::string_t &value) {
		std::string key(value.GetData(), value.GetSize());
		const auto it = lookup_.find(key);
		if (it != lookup_.end()) {
			return it->second;
		}
		const uint32_t id = static_cast<uint32_t>(storage_.size());
		lookup_.emplace(key, id);
		storage_.push_back(std::move(key));
		return id;
	}

	const std::string &Get(uint32_t id) const {
		return storage_[id];
	}

private:
	std::vector<std::string> storage_;
	std::unordered_map<std::string, uint32_t> lookup_;
};

void TagVersionTable::PopulateAuxPartition(const StringInterner &labels, SecEdgarQ4TagTlabelPartition &out) const {
	out.tlabel_ids_by_tag_version = tlabel_by_id_;
	uint32_t max_tlabel_id = 0;
	for (const auto tlabel_id : tlabel_by_id_) {
		max_tlabel_id = std::max(max_tlabel_id, tlabel_id);
	}
	out.tlabels.clear();
	out.tlabels.reserve(max_tlabel_id + 1);
	for (uint32_t id = 0; id <= max_tlabel_id; id++) {
		out.tlabels.push_back(labels.Get(id));
	}
	out.tag_version_lookup.clear();
	out.tag_version_lookup.reserve(lookup_.size());
	for (const auto &entry : lookup_) {
		out.tag_version_lookup.emplace(SecEdgarQ4TagVersionKey::FromStored(entry.first.tag, entry.first.version),
		                               entry.second);
	}
}

struct SubInfo {
	int32_t cik = 0;
	int32_t sic = 0;
	uint32_t cik_dense_id = 0;
};

struct GroupAggregate {
	int32_t sic = 0;
	uint32_t tlabel_id = 0;
	double sum = 0.0;
	int64_t count = 0;
	std::vector<uint64_t> cik_bits;
	uint32_t distinct_cik_count = 0;
	bool active = false;
};

struct ResultRow {
	int32_t sic = 0;
	std::string tlabel;
	double total_value = 0.0;
	double avg_value = 0.0;
	int64_t num_companies = 0;
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

void TrackChunkPath(SecEdgarQ4ProfileCounters *profile, duckdb::idx_t rows, int path) {
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

enum class NumTripleQualifyStatus : uint8_t { Unknown = 0, Qualifies = 1, Rejects = 2 };

struct AdshSubQualifyCache {
	std::vector<uint8_t> status;

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			status.reserve(estimate);
		}
	}

	bool Qualifies(duckdb::idx_t dict_idx, const duckdb::string_t &adsh,
	               const std::unordered_map<AdshKey, SubInfo, StringKeyHash> &sub_map) {
		if (dict_idx >= status.size()) {
			status.resize(dict_idx + 1, 0);
		}
		auto &slot = status[dict_idx];
		if (slot == 0) {
			slot = sub_map.find(AdshKey::From(adsh)) == sub_map.end() ? 2 : 1;
		}
		return slot == 1;
	}
};

struct NumTripleDictCache {
	const duckdb::SelectionVector *current_sel = nullptr;
	std::vector<NumTripleQualifyStatus> status;
	std::vector<AdshKey> keys;
	std::vector<ResolvedPreTriple> resolved;

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			status.reserve(estimate);
			keys.reserve(estimate);
			resolved.reserve(estimate);
		}
	}

	void BeginChunk(const duckdb::SelectionVector *sel) {
		if (current_sel != sel) {
			current_sel = sel;
			status.clear();
			keys.clear();
			resolved.clear();
		}
	}

	const ResolvedPreTriple *Lookup(duckdb::idx_t dict_idx, const duckdb::string_t &adsh,
	                                const duckdb::string_t &tag, const duckdb::string_t &version,
	                                const std::unordered_map<AdshKey, SubInfo, StringKeyHash> &sub_map,
	                                const std::unordered_map<AdshKey, PreEntriesByTagVersion, StringKeyHash> &pre_map,
	                                const TagVersionTable &tag_versions) {
		if (dict_idx >= status.size()) {
			status.resize(dict_idx + 1, NumTripleQualifyStatus::Unknown);
			keys.resize(dict_idx + 1);
			resolved.resize(dict_idx + 1);
		}
		const auto adsh_key = AdshKey::From(adsh);
		auto &slot_status = status[dict_idx];
		if (slot_status != NumTripleQualifyStatus::Unknown) {
			if (keys[dict_idx] == adsh_key) {
				return slot_status == NumTripleQualifyStatus::Qualifies ? &resolved[dict_idx] : nullptr;
			}
			slot_status = NumTripleQualifyStatus::Unknown;
		}
		keys[dict_idx] = adsh_key;
		if (sub_map.find(adsh_key) == sub_map.end()) {
			slot_status = NumTripleQualifyStatus::Rejects;
			return nullptr;
		}
		const auto adsh_it = pre_map.find(adsh_key);
		if (adsh_it == pre_map.end()) {
			slot_status = NumTripleQualifyStatus::Rejects;
			return nullptr;
		}
		uint32_t tag_version_id = 0;
		if (!tag_versions.Lookup(tag, version, tag_version_id)) {
			slot_status = NumTripleQualifyStatus::Rejects;
			return nullptr;
		}
		const auto entry_it = adsh_it->second.find(tag_version_id);
		if (entry_it == adsh_it->second.end() || entry_it->second.multiplicity == 0) {
			slot_status = NumTripleQualifyStatus::Rejects;
			return nullptr;
		}
		slot_status = NumTripleQualifyStatus::Qualifies;
		resolved[dict_idx] = entry_it->second;
		return &resolved[dict_idx];
	}

	const ResolvedPreTriple *LookupVerified(duckdb::idx_t dict_idx) const {
		if (dict_idx >= status.size()) {
			return nullptr;
		}
		if (status[dict_idx] == NumTripleQualifyStatus::Qualifies) {
			return &resolved[dict_idx];
		}
		return nullptr;
	}

	void Prewarm(const ConsumableStringColumn &adsh_col, const ConsumableStringColumn &tag_col,
	             const ConsumableStringColumn &version_col, duckdb::idx_t count,
	             const std::unordered_map<AdshKey, SubInfo, StringKeyHash> &sub_map,
	             const std::unordered_map<AdshKey, PreEntriesByTagVersion, StringKeyHash> &pre_map,
	             const TagVersionTable &tag_versions) {
		if (adsh_col.kind != ConsumableKind::Dict || tag_col.kind != ConsumableKind::Dict ||
		    version_col.kind != ConsumableKind::Dict) {
			return;
		}
		BeginChunk(adsh_col.sel);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto dict_idx = ConsumableRowIndex(adsh_col, row);
			if (dict_idx < status.size() && status[dict_idx] != NumTripleQualifyStatus::Unknown) {
				continue;
			}
			const auto tag_index = ConsumableRowIndex(tag_col, row);
			const auto version_index = ConsumableRowIndex(version_col, row);
			Lookup(dict_idx, adsh_col.data[dict_idx], tag_col.data[tag_index], version_col.data[version_index],
			       sub_map, pre_map, tag_versions);
		}
	}
};

struct ProfileAggregateUs {
	SecEdgarQ4ProfileCounters *counters = nullptr;
	Clock::time_point start {};

	explicit ProfileAggregateUs(SecEdgarQ4ProfileCounters *profile_counters)
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

struct SecEdgarQ4Kernel::State {
	explicit State(duckdb::idx_t row_estimate) {
		const auto reserve_groups = row_estimate > 0 ? row_estimate / 500 : 4096;
		const auto reserve_pre = row_estimate > 0 ? row_estimate / 20 : 65536;
		const auto reserve_sub = row_estimate > 0 ? row_estimate / 100 : 8192;
		groups.reserve(reserve_groups);
		group_index.reserve(reserve_groups);
		pre_by_adsh.reserve(reserve_pre);
		pending_pre.reserve(reserve_sub);
		sub_lookup.reserve(reserve_sub);
		cik_dense_index.reserve(reserve_sub);
		num_triple_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
		adsh_sub_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
	}

	void FinalizeSubLookup() {
		cik_bit_word_count = (cik_dense_count + 63) / 64;
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

	void CollectNeededTagVersions() {
		needed_tag_versions.clear();
		for (const auto &adsh_entry : pending_pre) {
			for (const auto &tv_entry : adsh_entry.second) {
				needed_tag_versions.insert(tv_entry.first);
			}
		}
		tag_scan_filter_active = !needed_tag_versions.empty();
	}

	void FinalizePreEntries() {
		for (const auto &adsh_entry : pending_pre) {
			const auto &adsh_key = adsh_entry.first;
			const auto sub_it = sub_lookup.find(adsh_key);
			if (sub_it == sub_lookup.end()) {
				continue;
			}
			for (const auto &tv_entry : adsh_entry.second) {
				uint32_t tag_version_id = 0;
				if (!tag_version_table.LookupKey(tv_entry.first, tag_version_id)) {
					continue;
				}
				auto &resolved = pre_by_adsh[adsh_key][tag_version_id];
				if (resolved.multiplicity == 0) {
					resolved.group_id =
					    GetOrCreateGroupId(sub_it->second.sic, tag_version_table.TlabelId(tag_version_id));
					resolved.cik_dense_id = sub_it->second.cik_dense_id;
				}
				resolved.multiplicity += tv_entry.second;
			}
		}
		pending_pre.clear();
	}

	const duckdb::TableFilterSet *AdshScanFilters() const {
		return adsh_bloom_ready ? adsh_scan_filters.get() : nullptr;
	}

	StringInterner tlabel_intern;
	TagVersionTable tag_version_table;
	std::unordered_map<AdshKey, SubInfo, StringKeyHash> sub_lookup;
	std::vector<std::string> sub_adsh_strings;
	duckdb::BloomFilter adsh_bloom;
	duckdb::unique_ptr<duckdb::TableFilterSet> adsh_scan_filters;
	bool adsh_bloom_ready = false;
	std::unordered_map<AdshKey, PendingPreByTagVersion, StringKeyHash> pending_pre;
	std::unordered_set<TagVersionKey, TagVersionKeyHash> needed_tag_versions;
	bool tag_scan_filter_active = false;
	std::unordered_map<int32_t, uint32_t> cik_dense_index;
	uint32_t cik_dense_count = 0;
	uint32_t cik_bit_word_count = 0;
	std::unordered_map<AdshKey, PreEntriesByTagVersion, StringKeyHash> pre_by_adsh;
	std::unordered_map<CompactGroupKey, uint32_t, CompactGroupKeyHash> group_index;
	std::vector<GroupAggregate> groups;
	NumTripleDictCache num_triple_cache;
	AdshSubQualifyCache adsh_sub_cache;
	std::vector<ResultRow> ordered_output;
	bool fused_scans_done = false;
	bool tag_abstract_filter_pushed = false;
	bool sub_sic_filter_pushed = false;
	bool pre_stmt_filter_pushed = false;
	bool num_uom_filter_pushed = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;

	void InsertTagRow(const duckdb::string_t &tag, const duckdb::string_t &version, int32_t abstract,
	                  const duckdb::string_t &tlabel) {
		if (!tag_abstract_filter_pushed && abstract != 0) {
			return;
		}
		const auto key = TagVersionKey::From(tag, version);
		if (tag_scan_filter_active && needed_tag_versions.find(key) == needed_tag_versions.end()) {
			return;
		}
		const auto tlabel_id = tlabel_intern.Intern(tlabel);
		tag_version_table.Intern(tag, version, tlabel_id);
	}

	void InsertSubRow(const duckdb::string_t &adsh, int32_t cik, int32_t sic) {
		if (!sub_sic_filter_pushed && (sic < kSicMin || sic > kSicMax)) {
			return;
		}
		const auto adsh_key = AdshKey::From(adsh);
		if (sub_lookup.find(adsh_key) != sub_lookup.end()) {
			return;
		}
		uint32_t dense_id = 0;
		const auto cik_it = cik_dense_index.find(cik);
		if (cik_it == cik_dense_index.end()) {
			dense_id = cik_dense_count++;
			cik_dense_index.emplace(cik, dense_id);
		} else {
			dense_id = cik_it->second;
		}
		sub_adsh_strings.emplace_back(adsh.GetData(), adsh.GetSize());
		sub_lookup.emplace(adsh_key, SubInfo {cik, sic, dense_id});
	}

	void InsertPreRow(const duckdb::string_t &adsh, const duckdb::string_t &tag, const duckdb::string_t &version,
	                  const duckdb::string_t &stmt) {
		if (!pre_stmt_filter_pushed) {
			if (stmt.GetSize() != 2 || stmt.GetData() == nullptr || stmt.GetData()[0] != 'E' || stmt.GetData()[1] != 'Q') {
				return;
			}
		}
		const auto adsh_key = AdshKey::From(adsh);
		if (sub_lookup.find(adsh_key) == sub_lookup.end()) {
			return;
		}
		pending_pre[adsh_key][TagVersionKey::From(tag, version)]++;
	}

	uint32_t GetOrCreateGroupId(int32_t sic, uint32_t tlabel_id) {
		const CompactGroupKey key {sic, tlabel_id};
		const auto it = group_index.find(key);
		if (it != group_index.end()) {
			return it->second;
		}
		const auto group_id = static_cast<uint32_t>(groups.size());
		groups.push_back(GroupAggregate {sic, tlabel_id, 0.0, 0, std::vector<uint64_t>(cik_bit_word_count, 0), 0, false});
		group_index.emplace(key, group_id);
		return group_id;
	}

	void Accumulate(uint32_t group_id, uint32_t cik_dense_id, double value, uint32_t pre_multiplicity,
	                SecEdgarQ4ProfileCounters *profile_counters) {
		auto &group = groups[group_id];
		if (!group.active) {
			group.active = true;
			if (group.cik_bits.size() != cik_bit_word_count) {
				group.cik_bits.assign(cik_bit_word_count, 0);
			}
			if (profile_counters) {
				profile_counters->distinct_inserts++;
			}
		}
		group.sum += value * static_cast<double>(pre_multiplicity);
		group.count += static_cast<int64_t>(pre_multiplicity);
		if (cik_bit_word_count > 0 && cik_dense_id < cik_dense_count) {
			const auto word = cik_dense_id >> 6;
			const auto bit = cik_dense_id & 63U;
			const auto mask = uint64_t(1) << bit;
			if ((group.cik_bits[word] & mask) == 0) {
				group.cik_bits[word] |= mask;
				group.distinct_cik_count++;
			}
		}
		if (profile_counters) {
			profile_counters->qualifying_rows += pre_multiplicity;
		}
	}

	bool ProcessQualifyingRow(const duckdb::string_t &adsh, const duckdb::string_t &tag,
	                          const duckdb::string_t &version, double value,
	                          SecEdgarQ4ProfileCounters *profile_counters) {
		const auto adsh_key = AdshKey::From(adsh);
		if (sub_lookup.find(adsh_key) == sub_lookup.end()) {
			return false;
		}
		const auto adsh_it = pre_by_adsh.find(adsh_key);
		if (adsh_it == pre_by_adsh.end()) {
			return false;
		}
		uint32_t tag_version_id = 0;
		if (!tag_version_table.Lookup(tag, version, tag_version_id)) {
			return false;
		}
		const auto entry_it = adsh_it->second.find(tag_version_id);
		if (entry_it == adsh_it->second.end() || entry_it->second.multiplicity == 0) {
			return false;
		}
		const auto &resolved = entry_it->second;
		Accumulate(resolved.group_id, resolved.cik_dense_id, value, resolved.multiplicity, profile_counters);
		return true;
	}

	void ProcessNumChunkVectors(duckdb::idx_t count, duckdb::Vector &adsh_vec, duckdb::Vector &tag_vec,
	                            duckdb::Vector &version_vec, duckdb::Vector &value_vec, duckdb::Vector &uom_vec,
	                            SecEdgarQ4ProfileCounters *profile_counters) {
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
				if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
					continue;
				}
				if (!tag_format.validity.AllValid() && !tag_format.validity.RowIsValidUnsafe(tag_index)) {
					continue;
				}
				if (!version_format.validity.AllValid() && !version_format.validity.RowIsValidUnsafe(version_index)) {
					continue;
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

		if (all_flat) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				ProcessQualifyingRow(adsh_col.data[row], tag_col.data[row], version_col.data[row], value_col.data[row],
				                     profile_counters);
			}
			return;
		}

		const bool shared_dict_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
		                             version_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
		                             version_col.sel == tag_col.sel;
		if (shared_dict_sel) {
			num_triple_cache.Prewarm(adsh_col, tag_col, version_col, count, sub_lookup, pre_by_adsh,
			                         tag_version_table);
			const bool value_dict_shared = value_col.kind == ConsumableKind::Dict && value_col.sel == adsh_col.sel;
			const auto &sel = *adsh_col.sel;
			const bool all_valid = adsh_col.all_valid && value_col.all_valid;
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto index = sel.get_index(row);
				if (!adsh_sub_cache.Qualifies(index, adsh_col.data[index], sub_lookup)) {
					continue;
				}
				const auto value_index = value_dict_shared ? index : ConsumableRowIndex(value_col, row);
				if (!all_valid &&
				    (!ConsumableEntryValid(adsh_col, index) || !ConsumableEntryValid(value_col, value_index))) {
					continue;
				}
				const auto *resolved = num_triple_cache.LookupVerified(index);
				if (resolved == nullptr) {
					continue;
				}
				Accumulate(resolved->group_id, resolved->cik_dense_id, value_col.data[value_index],
				           resolved->multiplicity, profile_counters);
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

	void ProcessTagChunk(duckdb::DataChunk &input) {
		if (input.size() == 0) {
			return;
		}
		auto &tag_vec = input.data[0];
		auto &version_vec = input.data[1];
		auto &abstract_vec = input.data[2];
		auto &tlabel_vec = input.data[3];
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		ConsumableInt32Column abstract_col;
		ConsumableStringColumn tlabel_col;
		if (BindConsumableString(tag_vec, tag_col) && BindConsumableString(version_vec, version_col) &&
		    BindConsumableInt32(abstract_vec, abstract_col) && BindConsumableString(tlabel_vec, tlabel_col)) {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto tag_index = ConsumableRowIndex(tag_col, row);
				const auto version_index = ConsumableRowIndex(version_col, row);
				const auto abstract_index = ConsumableRowIndex(abstract_col, row);
				const auto tlabel_index = ConsumableRowIndex(tlabel_col, row);
				if (!ConsumableEntryValid(tag_col, tag_index) || !ConsumableEntryValid(version_col, version_index) ||
				    !ConsumableEntryValid(abstract_col, abstract_index) || !ConsumableEntryValid(tlabel_col, tlabel_index)) {
					continue;
				}
				InsertTagRow(tag_col.data[tag_index], version_col.data[version_index], abstract_col.data[abstract_index],
				             tlabel_col.data[tlabel_index]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat version_format;
		duckdb::UnifiedVectorFormat abstract_format;
		duckdb::UnifiedVectorFormat tlabel_format;
		const auto count = input.size();
		tag_vec.ToUnifiedFormat(count, tag_format);
		version_vec.ToUnifiedFormat(count, version_format);
		abstract_vec.ToUnifiedFormat(count, abstract_format);
		tlabel_vec.ToUnifiedFormat(count, tlabel_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
		auto abstract_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(abstract_format);
		auto tlabel_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tlabel_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto tag_index = tag_format.sel->get_index(row);
			const auto version_index = version_format.sel->get_index(row);
			const auto abstract_index = abstract_format.sel->get_index(row);
			const auto tlabel_index = tlabel_format.sel->get_index(row);
			if (!abstract_format.validity.AllValid() && !abstract_format.validity.RowIsValidUnsafe(abstract_index)) {
				continue;
			}
			InsertTagRow(tag_values[tag_index], version_values[version_index], abstract_values[abstract_index],
			             tlabel_values[tlabel_index]);
		}
	}

	void ProcessSubChunk(duckdb::DataChunk &input) {
		if (input.size() == 0) {
			return;
		}
		auto &adsh_vec = input.data[0];
		auto &cik_vec = input.data[1];
		auto &sic_vec = input.data[2];
		ConsumableStringColumn adsh_col;
		ConsumableInt32Column cik_col;
		ConsumableInt32Column sic_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableInt32(cik_vec, cik_col) &&
		    BindConsumableInt32(sic_vec, sic_col)) {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto cik_index = ConsumableRowIndex(cik_col, row);
				const auto sic_index = ConsumableRowIndex(sic_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(cik_col, cik_index) ||
				    !ConsumableEntryValid(sic_col, sic_index)) {
					continue;
				}
				InsertSubRow(adsh_col.data[adsh_index], cik_col.data[cik_index], sic_col.data[sic_index]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat cik_format;
		duckdb::UnifiedVectorFormat sic_format;
		const auto count = input.size();
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		cik_vec.ToUnifiedFormat(count, cik_format);
		sic_vec.ToUnifiedFormat(count, sic_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto cik_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(cik_format);
		auto sic_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(sic_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto cik_index = cik_format.sel->get_index(row);
			const auto sic_index = sic_format.sel->get_index(row);
			if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
				continue;
			}
			if (!cik_format.validity.AllValid() && !cik_format.validity.RowIsValidUnsafe(cik_index)) {
				continue;
			}
			if (!sic_format.validity.AllValid() && !sic_format.validity.RowIsValidUnsafe(sic_index)) {
				continue;
			}
			InsertSubRow(adsh_values[adsh_index], cik_values[cik_index], sic_values[sic_index]);
		}
	}

	void ProcessPreChunk(duckdb::DataChunk &input) {
		if (input.size() == 0) {
			return;
		}
		auto &adsh_vec = input.data[0];
		auto &tag_vec = input.data[1];
		auto &version_vec = input.data[2];
		auto &stmt_vec = input.data[3];
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		ConsumableStringColumn stmt_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		    BindConsumableString(version_vec, version_col) && BindConsumableString(stmt_vec, stmt_col)) {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto tag_index = ConsumableRowIndex(tag_col, row);
				const auto version_index = ConsumableRowIndex(version_col, row);
				const auto stmt_index = ConsumableRowIndex(stmt_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(tag_col, tag_index) ||
				    !ConsumableEntryValid(version_col, version_index) || !ConsumableEntryValid(stmt_col, stmt_index)) {
					continue;
				}
				InsertPreRow(adsh_col.data[adsh_index], tag_col.data[tag_index], version_col.data[version_index],
				             stmt_col.data[stmt_index]);
			}
			return;
		}
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat version_format;
		duckdb::UnifiedVectorFormat stmt_format;
		const auto count = input.size();
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		tag_vec.ToUnifiedFormat(count, tag_format);
		version_vec.ToUnifiedFormat(count, version_format);
		stmt_vec.ToUnifiedFormat(count, stmt_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
		auto stmt_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(stmt_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto version_index = version_format.sel->get_index(row);
			const auto stmt_index = stmt_format.sel->get_index(row);
			InsertPreRow(adsh_values[adsh_index], tag_values[tag_index], version_values[version_index],
			             stmt_values[stmt_index]);
		}
	}
};

SecEdgarQ4Kernel::SecEdgarQ4Kernel(SecEdgarQ4Parameters params_p, SecEdgarQ4ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ4Kernel::~SecEdgarQ4Kernel() = default;

bool SecEdgarQ4Kernel::InputsConsumed() const {
	return state->fused_scans_done;
}

void SecEdgarQ4Kernel::MarkInputsConsumed() {
	state->fused_scans_done = true;
}

void SecEdgarQ4Kernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q4 kernel: use fused pre-scan path");
}

void SecEdgarQ4Kernel::RunTagScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		state->ProcessTagChunk(source->Chunk());
	}
	if (profile) {
		profile->tag_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ4Kernel::RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		state->ProcessSubChunk(source->Chunk());
	}
	state->FinalizeSubLookup();
	if (profile) {
		profile->sub_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ4Kernel::RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data, state->AdshScanFilters());
	while (source->Next()) {
		state->ProcessPreChunk(source->Chunk());
	}
	if (profile) {
		profile->pre_table_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ4Kernel::RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data, state->AdshScanFilters());
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		const auto consume_start = Clock::now();
		state->ProcessNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ4NumChunkLayout::ADSH_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::TAG_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::VERSION_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::VALUE_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::UOM_INDEX], profile);
		if (profile) {
			profile->pre_consume_us +=
			    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count();
		}
	}
	if (profile) {
		profile->num_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
		duckdb::idx_t active = 0;
		for (const auto &group : state->groups) {
			if (group.active) {
				active++;
			}
		}
		profile->active_groups = active;
	}
}

void SecEdgarQ4Kernel::FinalizeGroups() {
	if (state->finalized) {
		return;
	}
	state->ordered_output.clear();
	state->ordered_output.reserve(state->groups.size());
	for (const auto &group : state->groups) {
		if (!group.active || static_cast<int64_t>(group.distinct_cik_count) < kMinDistinctCik || group.count == 0) {
			continue;
		}
		ResultRow row;
		row.sic = group.sic;
		row.tlabel = state->tlabel_intern.Get(group.tlabel_id);
		row.total_value = group.sum;
		row.avg_value = group.sum / static_cast<double>(group.count);
		row.num_companies = static_cast<int64_t>(group.distinct_cik_count);
		state->ordered_output.push_back(std::move(row));
	}
	std::sort(state->ordered_output.begin(), state->ordered_output.end(),
	          [](const ResultRow &lhs, const ResultRow &rhs) {
		          if (lhs.total_value != rhs.total_value) {
			          return lhs.total_value > rhs.total_value;
		          }
		          if (lhs.sic != rhs.sic) {
			          return lhs.sic < rhs.sic;
		          }
		          return lhs.tlabel < rhs.tlabel;
	          });
	if (state->ordered_output.size() > kResultLimit) {
		state->ordered_output.resize(kResultLimit);
	}
	if (profile) {
		profile->candidate_rows = state->ordered_output.size();
	}
	state->finalized = true;
}

void SecEdgarQ4Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeGroups();
	if (profile) {
		profile->finalize_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

SecEdgarQ4AdshKey ToAuxAdshKey(const AdshKey &key) {
	SecEdgarQ4AdshKey out;
	out.size = key.size;
	out.w0 = key.w0;
	out.w1 = key.w1;
	out.w2 = key.w2;
	return out;
}

void SecEdgarQ4Kernel::ExportAuxPartitions(SecEdgarQ4SicCohortPartition &cohort, SecEdgarQ4PreEqBridgePartition &pre_bridge,
                                           SecEdgarQ4TagTlabelPartition &tag_tlabel,
                                           SecEdgarQ4GroupAggregatesPartition &aggregates, SecEdgarQ4OutputPartition &output) {
	if (!state->finalized) {
		FinalizeGroups();
	}

	cohort.sub_lookup.clear();
	cohort.cik_dense_index = state->cik_dense_index;
	cohort.cik_dense_count = state->cik_dense_count;
	cohort.cik_bit_word_count = state->cik_bit_word_count;
	cohort.sub_lookup.reserve(state->sub_lookup.size());
	for (const auto &entry : state->sub_lookup) {
		cohort.sub_lookup.emplace(ToAuxAdshKey(entry.first),
		                          SecEdgarQ4SubInfo {entry.second.cik, entry.second.sic, entry.second.cik_dense_id});
	}

	pre_bridge.pre_by_adsh.clear();
	pre_bridge.pre_by_adsh.reserve(state->pre_by_adsh.size());
	for (const auto &adsh_entry : state->pre_by_adsh) {
		SecEdgarQ4PreEntriesByTagVersion entries;
		entries.reserve(adsh_entry.second.size());
		for (const auto &tv_entry : adsh_entry.second) {
			entries.emplace(tv_entry.first,
			                SecEdgarQ4ResolvedPreTriple {tv_entry.second.group_id, tv_entry.second.cik_dense_id,
			                                               tv_entry.second.multiplicity});
		}
		pre_bridge.pre_by_adsh.emplace(ToAuxAdshKey(adsh_entry.first), std::move(entries));
	}

	state->tag_version_table.PopulateAuxPartition(state->tlabel_intern, tag_tlabel);

	aggregates.group_index.clear();
	aggregates.groups.clear();
	aggregates.group_index.reserve(state->group_index.size());
	aggregates.groups.reserve(state->groups.size());
	for (const auto &entry : state->group_index) {
		aggregates.group_index.emplace(SecEdgarQ4GroupKey {entry.first.sic, entry.first.tlabel_id}, entry.second);
	}
	for (const auto &group : state->groups) {
		aggregates.groups.push_back(SecEdgarQ4GroupAggregate {group.sic, group.tlabel_id, group.sum, group.count,
		                                                    group.cik_bits, group.distinct_cik_count, group.active});
	}

	output.output_rows.clear();
	output.output_rows.reserve(state->ordered_output.size());
	for (const auto &row : state->ordered_output) {
		output.output_rows.push_back(
		    SecEdgarQ4OutputRow {row.sic, row.tlabel, "EQ", row.num_companies, row.total_value, row.avg_value});
	}
}

bool SecEdgarQ4Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_output.size()) {
		return false;
	}
	const auto remaining = state->ordered_output.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_output[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value::INTEGER(result.sic));
		output.SetValue(1, row, duckdb::Value(result.tlabel));
		output.SetValue(2, row, duckdb::Value("EQ"));
		output.SetValue(3, row, duckdb::Value::BIGINT(result.num_companies));
		output.SetValue(4, row, duckdb::Value::DOUBLE(result.total_value));
		output.SetValue(5, row, duckdb::Value::DOUBLE(result.avg_value));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4Kernel(const SecEdgarQ4Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ4Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4ProfilingKernel(const SecEdgarQ4Parameters &params,
                                                                    SecEdgarQ4ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ4Kernel>(params, &counters);
}

SecEdgarQ4FusedKernel::SecEdgarQ4FusedKernel(const SecEdgarQ4Parameters &params, SecEdgarQ4ProfileCounters *profile_p)
    : SecEdgarQ4Kernel(params, profile_p) {
}

void SecEdgarQ4Kernel::RunPostgresChunkPrescan(const PostgresChunkScanner &scan) {
	if (InputsConsumed()) {
		return;
	}
	state->tag_abstract_filter_pushed = true;
	state->sub_sic_filter_pushed = true;
	state->pre_stmt_filter_pushed = true;
	state->num_uom_filter_pushed = true;
	scan(CreateSecEdgarQ4SubScanSpec(), [&](duckdb::DataChunk &chunk) { state->ProcessSubChunk(chunk); });
	scan(CreateSecEdgarQ4PreScanSpec(), [&](duckdb::DataChunk &chunk) { state->ProcessPreChunk(chunk); });
	state->CollectNeededTagVersions();
	if (!state->needed_tag_versions.empty()) {
		scan(CreateSecEdgarQ4TagScanSpec(), [&](duckdb::DataChunk &chunk) { state->ProcessTagChunk(chunk); });
	}
	state->FinalizePreEntries();
	scan(CreateSecEdgarQ4NumScanSpec(), [&](duckdb::DataChunk &chunk) {
		if (chunk.size() == 0) {
			return;
		}
		state->ProcessNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ4NumChunkLayout::ADSH_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::TAG_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::VERSION_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::VALUE_INDEX],
		                              chunk.data[SecEdgarQ4NumChunkLayout::UOM_INDEX], profile);
	});
	MarkInputsConsumed();
}

void SecEdgarQ4FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q4 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ4FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind) {
	if (InputsConsumed()) {
		return;
	}
	const auto scan_start = Clock::now();
	const auto tag_bind = BindNativeScan(context, CreateSecEdgarQ4TagScanSpec());
	const auto sub_bind = BindNativeScan(context, CreateSecEdgarQ4SubScanSpec());
	const auto pre_bind = BindNativeScan(context, CreateSecEdgarQ4PreScanSpec());
	state->tag_abstract_filter_pushed = NativeScanHasFilters(*tag_bind);
	state->sub_sic_filter_pushed = NativeScanHasFilters(*sub_bind);
	state->pre_stmt_filter_pushed = NativeScanHasFilters(*pre_bind);
	state->num_uom_filter_pushed = NativeScanHasFilters(num_bind);
	RunSubScan(context, *sub_bind);
	state->BuildAdshBloomFilter(context);
	RunPreScan(context, *pre_bind);
	state->CollectNeededTagVersions();
	if (!state->needed_tag_versions.empty()) {
		RunTagScan(context, *tag_bind);
	}
	state->FinalizePreEntries();
	RunNumFusedPass(context, num_bind);
	MarkInputsConsumed();
	if (profile) {
		profile->pre_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void RunSecEdgarQ4FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ4FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, num_bind);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4FusedKernel(const SecEdgarQ4Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ4FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ4FusedProfilingKernel(const SecEdgarQ4Parameters &params,
                                                                          SecEdgarQ4ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ4FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
