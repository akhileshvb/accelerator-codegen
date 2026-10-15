#include "backends/duckdb/engine/duckdb_scan_adapter.hpp"
#include "sec_edgar/q24/duckdb/base/query.hpp"
#include "sec_edgar/q24/duckdb/base/fused.hpp"
#include "sec_edgar/q24/duckdb/aux/finalize.hpp"

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
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/filter/bloom_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace accelerator_codegen {
namespace {

using Clock = std::chrono::steady_clock;

constexpr duckdb::idx_t kOutputBatchSize = 64;
constexpr duckdb::idx_t kResultLimit = 100;
constexpr int64_t kMinGroupCount = 11;
constexpr int32_t kDdateMin = 20230101;
constexpr int32_t kDdateMax = 20231231;

struct AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static AdshKey From(const duckdb::string_t &value) {
		AdshKey key;
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

	bool operator==(const AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct AdshKeyHash {
	size_t operator()(const AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

static bool StoredStringEquals(const std::string &stored, const duckdb::string_t &value) {
	return stored.size() == value.GetSize() &&
	       (stored.empty() || std::memcmp(stored.data(), value.GetData(), value.GetSize()) == 0);
}

class AdshInternTable {
public:
	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			lookup_.reserve(estimate);
		}
	}

	uint32_t Intern(const duckdb::string_t &adsh) {
		return InternKey(AdshKey::From(adsh));
	}

	uint32_t InternKey(const AdshKey &key) {
		const auto it = lookup_.find(key);
		if (it != lookup_.end()) {
			return it->second;
		}
		const uint32_t id = static_cast<uint32_t>(lookup_.size());
		lookup_.emplace(key, id);
		return id;
	}

	bool InternKeyNew(const AdshKey &key, uint32_t &out_id) {
		const auto it = lookup_.find(key);
		if (it != lookup_.end()) {
			out_id = it->second;
			return false;
		}
		out_id = static_cast<uint32_t>(lookup_.size());
		lookup_.emplace(key, out_id);
		return true;
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

struct TagVersionKey {
	AdshKey tag;
	AdshKey version;

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

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			lookup_.reserve(estimate);
			storage_.reserve(estimate);
		}
	}

	uint32_t Intern(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return InternKeys(AdshKey::From(tag), AdshKey::From(version), tag, version);
	}

	// The keys hold only the first 24 bytes of the tag / version (tags run to
	// ~100 bytes and distinct tags often share a 24-byte prefix), so an id is
	// returned only when its stored strings match in full.
	uint32_t InternKeys(const AdshKey &tag_key, const AdshKey &version_key, const duckdb::string_t &tag,
	                    const duckdb::string_t &version) {
		uint32_t found = 0;
		if (LookupKeys(tag_key, version_key, tag, version, found)) {
			return found;
		}
		const TagVersionKey key {tag_key, version_key};
		const uint32_t id = static_cast<uint32_t>(storage_.size());
		lookup_.emplace(key, id);
		storage_.push_back(
		    StoredPair {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())});
		return id;
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

	const std::string &Tag(uint32_t id) const {
		return storage_[id].tag;
	}

	const std::string &Version(uint32_t id) const {
		return storage_[id].version;
	}

	void PopulateAux(SecEdgarQ24TagVersionPartition &out) const {
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
			    SecEdgarQ24TagVersionKey::FromStored(stored.tag, stored.version), entry.second);
		}
	}

private:
	std::vector<StoredPair> storage_;
	std::unordered_multimap<TagVersionKey, uint32_t, TagVersionKeyHash> lookup_;
};

struct PreByAdshMembership {
	void Reserve(duckdb::idx_t adsh_estimate) {
		if (adsh_estimate > 0) {
			by_adsh_.reserve(adsh_estimate);
		}
	}

	void Insert(uint32_t adsh_id, uint32_t tv_id) {
		if (by_adsh_.size() <= adsh_id) {
			by_adsh_.resize(adsh_id + 1);
		}
		by_adsh_[adsh_id].push_back(tv_id);
	}

	void FinalizeForLookup() {
		for (auto &tvs : by_adsh_) {
			if (tvs.size() <= 1) {
				continue;
			}
			std::sort(tvs.begin(), tvs.end());
			tvs.erase(std::unique(tvs.begin(), tvs.end()), tvs.end());
		}
	}

	bool Contains(uint32_t adsh_id, uint32_t tv_id) const {
		if (adsh_id >= by_adsh_.size()) {
			return false;
		}
		const auto &tvs = by_adsh_[adsh_id];
		if (tvs.size() <= 4) {
			for (const auto candidate : tvs) {
				if (candidate == tv_id) {
					return true;
				}
			}
			return false;
		}
		return std::binary_search(tvs.begin(), tvs.end(), tv_id);
	}

	const std::vector<std::vector<uint32_t>> &ByAdsh() const {
		return by_adsh_;
	}

private:
	std::vector<std::vector<uint32_t>> by_adsh_;
};

const duckdb::string_t &UsdUomConstant() {
	static const duckdb::string_t kUsd = duckdb::string_t("USD");
	return kUsd;
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

bool DdateQualifies(const ConsumableInt32Column &ddate_col, duckdb::idx_t row, bool filter_pushed) {
	if (filter_pushed) {
		return true;
	}
	const auto index = ConsumableRowIndex(ddate_col, row);
	if (!ConsumableEntryValid(ddate_col, index)) {
		return false;
	}
	const auto ddate = ddate_col.data[index];
	return ddate >= kDdateMin && ddate <= kDdateMax;
}

void TrackPreChunkPath(SecEdgarQ24ProfileCounters *profile, duckdb::idx_t rows, int path) {
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

void TrackNumChunkPath(SecEdgarQ24ProfileCounters *profile, duckdb::idx_t rows, int path) {
	if (!profile) {
		return;
	}
	if (path == 0) {
		profile->flat_chunks++;
	} else if (path == 1) {
		profile->dict_chunks++;
	} else {
		profile->generic_chunks++;
		profile->generic_fallback_rows += rows;
	}
}

struct GroupAggregate {
	int64_t cnt = 0;
	double total = 0.0;
};

struct ResultRow {
	uint32_t tv_id = 0;
	int64_t cnt = 0;
	double total = 0.0;
};

struct ProfileAggregateUs {
	SecEdgarQ24ProfileCounters *counters = nullptr;
	Clock::time_point start {};

	explicit ProfileAggregateUs(SecEdgarQ24ProfileCounters *profile_counters)
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

SecEdgarQ24AdshKey ToAuxAdshKey(const AdshKey &key) {
	SecEdgarQ24AdshKey out;
	out.size = key.size;
	out.w0 = key.w0;
	out.w1 = key.w1;
	out.w2 = key.w2;
	return out;
}

} // namespace

struct PreTripleDictCache {
	std::vector<uint8_t> inserted;

	void BeginChunk() {
		inserted.clear();
	}

	void InsertTriple(duckdb::idx_t dict_idx, const duckdb::string_t &adsh, const duckdb::string_t &tag,
	                  const duckdb::string_t &version, AdshInternTable &adsh_intern, TagVersionTable &tag_version_table,
	                  PreByAdshMembership &pre_membership) {
		if (dict_idx >= inserted.size()) {
			inserted.resize(dict_idx + 1, 0);
		}
		if (inserted[dict_idx]) {
			return;
		}
		inserted[dict_idx] = 1;
		const auto adsh_id = adsh_intern.Intern(adsh);
		const auto tv_id = tag_version_table.Intern(tag, version);
		pre_membership.Insert(adsh_id, tv_id);
	}
};

struct NumBufferDictCache {
	std::vector<uint8_t> valid;
	std::vector<uint32_t> adsh_ids;
	std::vector<uint32_t> tv_ids;

	// Slots are indexes into this chunk's dictionary and are valid for this
	// chunk only.  (They used to be kept while the selection vector's address
	// repeated, but a later chunk's dictionary buffer can be allocated at a
	// freed one's address with other strings behind the same indexes.)
	void BeginChunk(const duckdb::SelectionVector *) {
		valid.clear();
		adsh_ids.clear();
		tv_ids.clear();
	}

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			valid.reserve(estimate);
			adsh_ids.reserve(estimate);
			tv_ids.reserve(estimate);
		}
	}

	bool Resolve(duckdb::idx_t dict_idx, const duckdb::string_t &adsh, const duckdb::string_t &tag,
	             const duckdb::string_t &version, uint32_t &adsh_id_out, uint32_t &tv_id_out,
	             AdshInternTable &adsh_intern, TagVersionTable &tag_version_table) {
		if (dict_idx >= valid.size()) {
			valid.resize(dict_idx + 1, 0);
			adsh_ids.resize(dict_idx + 1);
			tv_ids.resize(dict_idx + 1);
		}
		if (valid[dict_idx]) {
			adsh_id_out = adsh_ids[dict_idx];
			tv_id_out = tv_ids[dict_idx];
			return false;
		}
		valid[dict_idx] = 1;
		const bool new_adsh = adsh_intern.InternKeyNew(AdshKey::From(adsh), adsh_id_out);
		const AdshKey tag_key = AdshKey::From(tag);
		const AdshKey version_key = AdshKey::From(version);
		tv_id_out = tag_version_table.InternKeys(tag_key, version_key, tag, version);
		adsh_ids[dict_idx] = adsh_id_out;
		tv_ids[dict_idx] = tv_id_out;
		return new_adsh;
	}
};

struct NumRowBuffer {
	std::vector<uint32_t> adsh_ids;
	std::vector<uint32_t> tv_ids;
	std::vector<double> values;
	duckdb::idx_t size = 0;

	void Reserve(duckdb::idx_t capacity) {
		if (capacity == 0) {
			return;
		}
		adsh_ids.resize(capacity);
		tv_ids.resize(capacity);
		values.resize(capacity);
	}

	void Append(uint32_t adsh_id, uint32_t tv_id, double value) {
		adsh_ids[size] = adsh_id;
		tv_ids[size] = tv_id;
		values[size] = value;
		size++;
	}
};

struct SecEdgarQ24Kernel::State {
	explicit State(duckdb::idx_t row_estimate) {
		const auto reserve_pre = row_estimate > 0 ? row_estimate / 2 : 1'048'576;
		const auto reserve_groups = row_estimate > 0 ? row_estimate / 200 : 4096;
		adsh_intern.Reserve(reserve_pre / 4);
		tag_version_table.Reserve(reserve_pre / 8);
		pre_membership.Reserve(reserve_pre / 4);
		groups.reserve(reserve_groups);
		num_buffer_dict_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
		num_row_buffer.Reserve(row_estimate > 0 ? row_estimate : 8'997'486);
		num_adsh_strings.reserve(reserve_pre / 4);
	}

	uint32_t InternAdshTrackBloom(const duckdb::string_t &adsh) {
		const AdshKey key = AdshKey::From(adsh);
		uint32_t adsh_id = 0;
		if (adsh_intern.InternKeyNew(key, adsh_id)) {
			num_adsh_strings.emplace_back(adsh.GetData(), adsh.GetSize());
		}
		return adsh_id;
	}

	void BuildAdshBloomFilter(duckdb::ClientContext &context) {
		if (num_adsh_strings.empty()) {
			return;
		}
		adsh_bloom.Initialize(context, num_adsh_strings.size());
		duckdb::Vector adsh_values(duckdb::LogicalType::VARCHAR, num_adsh_strings.size());
		adsh_values.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
		auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_values);
		for (duckdb::idx_t index = 0; index < num_adsh_strings.size(); index++) {
			adsh_data[index] = duckdb::string_t(num_adsh_strings[index]);
		}
		duckdb::Vector hashes(duckdb::LogicalType::HASH, num_adsh_strings.size());
		duckdb::VectorOperations::Hash(adsh_values, hashes, num_adsh_strings.size());
		adsh_bloom.InsertHashes(hashes, num_adsh_strings.size());
		adsh_scan_filters = duckdb::make_uniq<duckdb::TableFilterSet>();
		adsh_scan_filters->PushFilter(duckdb::ColumnIndex(0),
		                              duckdb::make_uniq<duckdb::BFTableFilter>(adsh_bloom, true, "adsh",
		                                                                       duckdb::LogicalType::VARCHAR));
		adsh_bloom_ready = true;
	}

	duckdb::unique_ptr<duckdb::TableFilterSet> CopyAdshScanFilters() const {
		if (!adsh_bloom_ready || !adsh_scan_filters) {
			return nullptr;
		}
		return adsh_scan_filters->Copy();
	}

	const duckdb::TableFilterSet *AdshScanFilters() const {
		return adsh_bloom_ready ? adsh_scan_filters.get() : nullptr;
	}

	void InsertPreTriple(const duckdb::string_t &adsh, const duckdb::string_t &tag, const duckdb::string_t &version) {
		const auto adsh_id = adsh_intern.Intern(adsh);
		const auto tv_id = tag_version_table.Intern(tag, version);
		pre_membership.Insert(adsh_id, tv_id);
	}

	void ProcessPreChunkVectors(duckdb::idx_t count, duckdb::Vector &adsh_vec, duckdb::Vector &tag_vec,
	                            duckdb::Vector &version_vec, SecEdgarQ24ProfileCounters *profile_counters) {
		if (count == 0) {
			return;
		}
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		    BindConsumableString(version_vec, version_col)) {
			const bool all_flat = adsh_col.kind == ConsumableKind::Flat && tag_col.kind == ConsumableKind::Flat &&
			                      version_col.kind == ConsumableKind::Flat && adsh_col.all_valid &&
			                      tag_col.all_valid && version_col.all_valid;
			TrackPreChunkPath(profile_counters, count, all_flat ? 0 : 1);
			if (all_flat) {
				for (duckdb::idx_t row = 0; row < count; row++) {
					InsertPreTriple(adsh_col.data[row], tag_col.data[row], version_col.data[row]);
				}
				return;
			}
			const bool shared_dict_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
			                             version_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
			                             version_col.sel == tag_col.sel && adsh_col.all_valid && tag_col.all_valid &&
			                             version_col.all_valid;
			if (shared_dict_sel) {
				pre_dict_cache.BeginChunk();
				const auto &sel = *adsh_col.sel;
				for (duckdb::idx_t row = 0; row < count; row++) {
					const auto dict_idx = sel.get_index(row);
					pre_dict_cache.InsertTriple(dict_idx, adsh_col.data[dict_idx], tag_col.data[dict_idx],
					                            version_col.data[dict_idx], adsh_intern, tag_version_table,
					                            pre_membership);
				}
				return;
			}
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto tag_index = ConsumableRowIndex(tag_col, row);
				const auto version_index = ConsumableRowIndex(version_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(tag_col, tag_index) ||
				    !ConsumableEntryValid(version_col, version_index)) {
					continue;
				}
				InsertPreTriple(adsh_col.data[adsh_index], tag_col.data[tag_index], version_col.data[version_index]);
			}
			return;
		}
		TrackPreChunkPath(profile_counters, count, 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat tag_format;
		duckdb::UnifiedVectorFormat version_format;
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		tag_vec.ToUnifiedFormat(count, tag_format);
		version_vec.ToUnifiedFormat(count, version_format);
		const auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		const auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
		const auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
			const auto tag_index = tag_format.sel->get_index(row);
			const auto version_index = version_format.sel->get_index(row);
			if (!adsh_format.validity.RowIsValid(adsh_index) || !tag_format.validity.RowIsValid(tag_index) ||
			    !version_format.validity.RowIsValid(version_index)) {
				continue;
			}
			InsertPreTriple(adsh_values[adsh_index], tag_values[tag_index], version_values[version_index]);
		}
	}

	void ProcessPreChunk(duckdb::DataChunk &chunk, SecEdgarQ24ProfileCounters *profile_counters) {
		if (chunk.size() == 0) {
			return;
		}
		ProcessPreChunkVectors(chunk.size(), chunk.data[SecEdgarQ24PreChunkLayout::ADSH_INDEX],
		                       chunk.data[SecEdgarQ24PreChunkLayout::TAG_INDEX],
		                       chunk.data[SecEdgarQ24PreChunkLayout::VERSION_INDEX], profile_counters);
	}

	void AccumulateTvId(uint32_t tv_id, double value, SecEdgarQ24ProfileCounters *profile_counters) {
		if (profile_counters) {
			profile_counters->qualifying_rows++;
		}
		if (groups.size() <= tv_id) {
			groups.resize(tv_id + 1);
		}
		auto &group = groups[tv_id];
		if (group.cnt == 0 && profile_counters) {
			profile_counters->distinct_inserts++;
		}
		group.cnt++;
		group.total += value;
	}

	void BufferNumRow(const duckdb::string_t &adsh, const duckdb::string_t &tag, const duckdb::string_t &version,
	                  double value) {
		const auto adsh_id = InternAdshTrackBloom(adsh);
		const AdshKey tag_key = AdshKey::From(tag);
		const AdshKey version_key = AdshKey::From(version);
		uint32_t tv_id = 0;
		tv_id = tag_version_table.InternKeys(tag_key, version_key, tag, version);
		AppendBufferRow(adsh_id, tv_id, value);
	}

	void AppendBufferRow(uint32_t adsh_id, uint32_t tv_id, double value) __attribute__((always_inline)) {
		num_row_buffer.Append(adsh_id, tv_id, value);
	}

	void BufferNumChunkVectors(duckdb::idx_t count, duckdb::Vector &adsh_vec, duckdb::Vector &tag_vec,
	                           duckdb::Vector &version_vec, duckdb::Vector &value_vec, duckdb::Vector &uom_vec,
	                           duckdb::Vector &ddate_vec) {
		ConsumableStringColumn adsh_col;
		ConsumableStringColumn tag_col;
		ConsumableStringColumn version_col;
		ConsumableDoubleColumn value_col;
		ConsumableStringColumn uom_col;
		ConsumableInt32Column ddate_col;
		if (!(BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(tag_vec, tag_col) &&
		      BindConsumableString(version_vec, version_col) && BindConsumableDouble(value_vec, value_col))) {
			duckdb::UnifiedVectorFormat adsh_format;
			duckdb::UnifiedVectorFormat tag_format;
			duckdb::UnifiedVectorFormat version_format;
			duckdb::UnifiedVectorFormat value_format;
			duckdb::UnifiedVectorFormat uom_format;
			duckdb::UnifiedVectorFormat ddate_format;
			adsh_vec.ToUnifiedFormat(count, adsh_format);
			tag_vec.ToUnifiedFormat(count, tag_format);
			version_vec.ToUnifiedFormat(count, version_format);
			value_vec.ToUnifiedFormat(count, value_format);
			uom_vec.ToUnifiedFormat(count, uom_format);
			ddate_vec.ToUnifiedFormat(count, ddate_format);
			const auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
			const auto tag_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(tag_format);
			const auto version_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(version_format);
			const auto value_data = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
			const auto uom_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(uom_format);
			const auto ddate_data = duckdb::UnifiedVectorFormat::GetData<int32_t>(ddate_format);
			for (duckdb::idx_t row = 0; row < count; row++) {
				const auto adsh_index = adsh_format.sel->get_index(row);
				const auto tag_index = tag_format.sel->get_index(row);
				const auto version_index = version_format.sel->get_index(row);
				const auto value_index = value_format.sel->get_index(row);
				const auto uom_index = uom_format.sel->get_index(row);
				const auto ddate_index = ddate_format.sel->get_index(row);
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
				if (!num_ddate_filter_pushed) {
					if (!ddate_format.validity.AllValid() && !ddate_format.validity.RowIsValidUnsafe(ddate_index)) {
						continue;
					}
					const auto ddate = ddate_data[ddate_index];
					if (ddate < kDdateMin || ddate > kDdateMax) {
						continue;
					}
				}
				BufferNumRow(adsh_values[adsh_index], tag_values[tag_index], version_values[version_index],
				             value_data[value_index]);
			}
			return;
		}

		if (num_uom_filter_pushed) {
			uom_col.kind = ConsumableKind::Constant;
			uom_col.data = &UsdUomConstant();
			uom_col.all_valid = true;
		} else if (!BindConsumableString(uom_vec, uom_col)) {
			return;
		}
		if (!num_ddate_filter_pushed && !BindConsumableInt32(ddate_vec, ddate_col)) {
			return;
		}

		if (UomChunkNeverUsd(uom_col)) {
			return;
		}
		const bool uom_always_usd = num_uom_filter_pushed || UomChunkAlwaysUsd(uom_col);
		const bool ddate_always_ok = num_ddate_filter_pushed;
		const bool shared_dict_sel = adsh_col.kind == ConsumableKind::Dict && tag_col.kind == ConsumableKind::Dict &&
		                             version_col.kind == ConsumableKind::Dict && adsh_col.sel == tag_col.sel &&
		                             version_col.sel == tag_col.sel;
		if (shared_dict_sel) {
			num_buffer_dict_cache.BeginChunk(adsh_col.sel);
			const bool value_dict_shared = value_col.kind == ConsumableKind::Dict && value_col.sel == adsh_col.sel;
			const auto &sel = *adsh_col.sel;
			const bool all_valid = adsh_col.all_valid && value_col.all_valid;
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				if (!ddate_always_ok && !DdateQualifies(ddate_col, row, num_ddate_filter_pushed)) {
					continue;
				}
				const auto index = sel.get_index(row);
				const auto value_index = value_dict_shared ? index : ConsumableRowIndex(value_col, row);
				if (!all_valid &&
				    (!ConsumableEntryValid(adsh_col, index) || !ConsumableEntryValid(value_col, value_index))) {
					continue;
				}
				uint32_t adsh_id = 0;
				uint32_t tv_id = 0;
				if (num_buffer_dict_cache.Resolve(index, adsh_col.data[index], tag_col.data[index],
				                                  version_col.data[index], adsh_id, tv_id, adsh_intern,
				                                  tag_version_table)) {
					num_adsh_strings.emplace_back(adsh_col.data[index].GetData(), adsh_col.data[index].GetSize());
				}
				AppendBufferRow(adsh_id, tv_id, value_col.data[value_index]);
			}
			return;
		}
		const bool all_flat = adsh_col.kind == ConsumableKind::Flat && tag_col.kind == ConsumableKind::Flat &&
		                      version_col.kind == ConsumableKind::Flat && value_col.kind == ConsumableKind::Flat &&
		                      adsh_col.all_valid && tag_col.all_valid && version_col.all_valid && value_col.all_valid;
		if (all_flat) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				if (!ddate_always_ok && !DdateQualifies(ddate_col, row, num_ddate_filter_pushed)) {
					continue;
				}
				BufferNumRow(adsh_col.data[row], tag_col.data[row], version_col.data[row], value_col.data[row]);
			}
			return;
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!uom_always_usd && !UomQualifies(uom_col, row)) {
				continue;
			}
			if (!ddate_always_ok && !DdateQualifies(ddate_col, row, num_ddate_filter_pushed)) {
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
			BufferNumRow(adsh_col.data[adsh_index], tag_col.data[tag_index], version_col.data[version_index],
			             value_col.data[value_index]);
		}
	}

	void ProcessBufferedNumRows(SecEdgarQ24ProfileCounters *profile_counters) {
		ProfileAggregateUs aggregate_timer(profile_counters);
		const auto *adsh_ids = num_row_buffer.adsh_ids.data();
		const auto *tv_ids = num_row_buffer.tv_ids.data();
		const auto *values = num_row_buffer.values.data();
		for (duckdb::idx_t row = 0; row < num_row_buffer.size; row++) {
			if (pre_membership.Contains(adsh_ids[row], tv_ids[row])) {
				continue;
			}
			AccumulateTvId(tv_ids[row], values[row], profile_counters);
		}
	}

	AdshInternTable adsh_intern;
	TagVersionTable tag_version_table;
	PreByAdshMembership pre_membership;
	PreTripleDictCache pre_dict_cache;
	std::vector<GroupAggregate> groups;
	std::vector<ResultRow> ordered_output;
	NumBufferDictCache num_buffer_dict_cache;
	NumRowBuffer num_row_buffer;
	duckdb::idx_t output_offset = 0;
	bool inputs_consumed = false;
	bool finalized = false;
	bool num_uom_filter_pushed = false;
	bool num_ddate_filter_pushed = false;
	std::vector<std::string> num_adsh_strings;
	duckdb::BloomFilter adsh_bloom;
	duckdb::unique_ptr<duckdb::TableFilterSet> adsh_scan_filters;
	bool adsh_bloom_ready = false;
};

SecEdgarQ24Kernel::SecEdgarQ24Kernel(SecEdgarQ24Parameters params_p, SecEdgarQ24ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ24Kernel::~SecEdgarQ24Kernel() = default;

bool SecEdgarQ24Kernel::InputsConsumed() const {
	return state->inputs_consumed;
}

void SecEdgarQ24Kernel::MarkInputsConsumed() {
	state->inputs_consumed = true;
}

void SecEdgarQ24Kernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q24 kernel: use fused prescan path");
}

void SecEdgarQ24Kernel::RunPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
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

void SecEdgarQ24Kernel::RunNumBufferPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		state->BufferNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ24NumChunkLayout::ADSH_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::TAG_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::VERSION_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::VALUE_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::UOM_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::DDATE_INDEX]);
	}
	if (profile) {
		profile->num_scan_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ24Kernel::FinalizeGroups() {
	if (state->finalized) {
		return;
	}
	state->ordered_output.clear();
	duckdb::idx_t active_groups = 0;
	for (uint32_t tv_id = 0; tv_id < state->groups.size(); tv_id++) {
		const auto &group = state->groups[tv_id];
		if (group.cnt == 0) {
			continue;
		}
		active_groups++;
		if (group.cnt < kMinGroupCount) {
			continue;
		}
		state->ordered_output.push_back(ResultRow {tv_id, group.cnt, group.total});
	}
	std::partial_sort(state->ordered_output.begin(),
	                  state->ordered_output.begin() +
	                      std::min<duckdb::idx_t>(state->ordered_output.size(), kResultLimit),
	                  state->ordered_output.end(), [](const ResultRow &lhs, const ResultRow &rhs) {
		                  if (lhs.cnt != rhs.cnt) {
			                  return lhs.cnt > rhs.cnt;
		                  }
		                  if (lhs.tv_id != rhs.tv_id) {
			                  return lhs.tv_id < rhs.tv_id;
		                  }
		                  return lhs.total > rhs.total;
	                  });
	if (state->ordered_output.size() > kResultLimit) {
		state->ordered_output.resize(kResultLimit);
	}
	if (profile) {
		profile->active_groups = active_groups;
		profile->candidate_rows = state->ordered_output.size();
	}
	state->finalized = true;
}

void SecEdgarQ24Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeGroups();
	if (profile) {
		profile->finalize_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

bool SecEdgarQ24Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_output.size()) {
		return false;
	}
	const auto remaining = state->ordered_output.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_output[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(state->tag_version_table.Tag(result.tv_id)));
		output.SetValue(1, row, duckdb::Value(state->tag_version_table.Version(result.tv_id)));
		output.SetValue(2, row, duckdb::Value::BIGINT(result.cnt));
		output.SetValue(3, row, duckdb::Value::DOUBLE(result.total));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

void SecEdgarQ24Kernel::ExportAuxPartitions(SecEdgarQ24PreMembershipPartition &pre_membership,
                                            SecEdgarQ24TagVersionPartition &tag_versions,
                                            SecEdgarQ24GroupAggregatesPartition &aggregates, SecEdgarQ24OutputPartition &output) {
	if (!state->finalized) {
		FinalizeGroups();
	}

	state->tag_version_table.PopulateAux(tag_versions);

	pre_membership.pre_by_adsh.clear();
	state->adsh_intern.ForEach([&](const AdshKey &adsh_key, uint32_t adsh_id) {
		if (adsh_id >= state->pre_membership.ByAdsh().size() || state->pre_membership.ByAdsh()[adsh_id].empty()) {
			return;
		}
		pre_membership.pre_by_adsh.emplace(ToAuxAdshKey(adsh_key), state->pre_membership.ByAdsh()[adsh_id]);
	});

	aggregates.groups.clear();
	aggregates.groups.reserve(state->groups.size());
	for (const auto &group : state->groups) {
		aggregates.groups.push_back(SecEdgarQ24GroupAggregate {group.cnt, group.total});
	}

	output.output_rows.clear();
	output.output_rows.reserve(state->ordered_output.size());
	for (const auto &row : state->ordered_output) {
		output.output_rows.push_back(
		    SecEdgarQ24OutputRow {state->tag_version_table.Tag(row.tv_id), state->tag_version_table.Version(row.tv_id),
		                          row.cnt, row.total});
	}
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24Kernel(const SecEdgarQ24Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ24Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24ProfilingKernel(const SecEdgarQ24Parameters &params,
                                                                    SecEdgarQ24ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ24Kernel>(params, &counters);
}

SecEdgarQ24FusedKernel::SecEdgarQ24FusedKernel(const SecEdgarQ24Parameters &params,
                                               SecEdgarQ24ProfileCounters *profile_p)
    : SecEdgarQ24Kernel(params, profile_p) {
}

void SecEdgarQ24Kernel::RunPostgresChunkPrescan(const PostgresChunkScanner &scan) {
	if (InputsConsumed()) {
		return;
	}
	state->num_uom_filter_pushed = true;
	state->num_ddate_filter_pushed = true;
	scan(CreateSecEdgarQ24NumFusedScanSpec(), [&](duckdb::DataChunk &chunk) {
		if (chunk.size() == 0) {
			return;
		}
		state->BufferNumChunkVectors(chunk.size(), chunk.data[SecEdgarQ24NumChunkLayout::ADSH_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::TAG_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::VERSION_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::VALUE_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::UOM_INDEX],
		                             chunk.data[SecEdgarQ24NumChunkLayout::DDATE_INDEX]);
	});
	scan(CreateSecEdgarQ24PreScanSpec(), [&](duckdb::DataChunk &chunk) {
		if (chunk.size() == 0) {
			return;
		}
		state->ProcessPreChunk(chunk, profile);
	});
	state->pre_membership.FinalizeForLookup();
	state->ProcessBufferedNumRows(profile);
	MarkInputsConsumed();
}

void SecEdgarQ24FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q24 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ24FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind) {
	(void)num_bind;
	if (InputsConsumed()) {
		return;
	}
	const auto scan_start = Clock::now();
	const auto num_bind_data = BindNativeScan(context, CreateSecEdgarQ24NumFusedScanSpec());
	state->num_uom_filter_pushed = NativeScanHasFilters(*num_bind_data);
	state->num_ddate_filter_pushed = NativeScanHasFilters(*num_bind_data);
	RunNumBufferPass(context, *num_bind_data);
	state->BuildAdshBloomFilter(context);
	const auto pre_bind = BindNativeScan(context, CreateSecEdgarQ24PreScanSpec());
	const auto pre_scan_start = Clock::now();
	RunPreScan(context, *pre_bind);
	state->pre_membership.FinalizeForLookup();
	if (profile) {
		profile->pre_table_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - pre_scan_start).count();
	}
	state->ProcessBufferedNumRows(profile);
	MarkInputsConsumed();
	if (profile) {
		profile->pre_scan_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void RunSecEdgarQ24FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                                const duckdb::FunctionData &num_bind) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ24FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, num_bind);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24FusedKernel(const SecEdgarQ24Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ24FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ24FusedProfilingKernel(const SecEdgarQ24Parameters &params,
                                                                         SecEdgarQ24ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ24FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
