#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "sec_edgar/q3/duckdb/base/query.hpp"
#include "sec_edgar/q3/duckdb/base/fused.hpp"

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
constexpr duckdb::idx_t kSubCikScanIndex = 2;
constexpr duckdb::idx_t kSubFyScanIndex = 3;

constexpr int32_t kTargetFy = 2022;

const duckdb::string_t &UsdUomConstant() {
	static const duckdb::string_t kUsd = duckdb::string_t("USD");
	return kUsd;
}

constexpr duckdb::idx_t kNumAdshScanIndex = 0;
constexpr duckdb::idx_t kNumValueScanIndex = 1;
constexpr duckdb::idx_t kNumUomScanIndex = 2;

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

struct FilingInfo {
	uint32_t group_id = 0;
	int32_t cik = 0;
};

struct GroupMeta {
	std::string name;
	int32_t cik = 0;
	bool touched = false;
};

struct NameCikKey {
	std::string name;
	int32_t cik = 0;

	bool operator==(const NameCikKey &other) const {
		return cik == other.cik && name == other.name;
	}
};

struct NameCikKeyHash {
	size_t operator()(const NameCikKey &key) const noexcept {
		return std::hash<std::string> {}(key.name) ^ (static_cast<size_t>(key.cik) << 1);
	}
};

struct ResultRow {
	std::string name;
	int32_t cik = 0;
	double total_value = 0.0;
};

struct NumRowBuffer {
	std::string adsh;
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

duckdb::idx_t SafeDictionarySize(duckdb::Vector &vector) {
	const auto dictionary_size = duckdb::DictionaryVector::DictionarySize(vector);
	if (dictionary_size.IsValid()) {
		return dictionary_size.GetIndex();
	}
	return 0;
}

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

int ConsumableChunkPath(const ConsumableStringColumn &adsh, const ConsumableDoubleColumn &value) {
	if (adsh.kind == ConsumableKind::Flat && value.kind == ConsumableKind::Flat && adsh.all_valid && value.all_valid) {
		return 0;
	}
	return 1;
}

int ConsumableChunkPath(const ConsumableStringColumn &adsh, const ConsumableStringColumn &name,
                        const ConsumableInt32Column &cik) {
	if (adsh.kind == ConsumableKind::Flat && name.kind == ConsumableKind::Flat && cik.kind == ConsumableKind::Flat &&
	    adsh.all_valid && name.all_valid && cik.all_valid) {
		return 0;
	}
	return 1;
}

void TrackChunkPath(SecEdgarQ3ProfileCounters *profile, duckdb::idx_t rows, int path) {
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

enum class AdshQualifyStatus : uint8_t { Unknown = 0, Qualifies = 1, Rejects = 2 };

struct QualifyingGroup {
	uint32_t group_id = 0;
	int32_t cik = 0;
};

struct AdshDictQualifyingCache {
	const duckdb::string_t *current_dict = nullptr;
	std::vector<AdshQualifyStatus> status;
	std::vector<AdshKey> keys;
	std::vector<QualifyingGroup> groups;

	void Reserve(duckdb::idx_t estimate) {
		if (estimate > 0) {
			status.reserve(estimate);
			keys.reserve(estimate);
			groups.reserve(estimate);
		}
	}

	void BeginChunk(const duckdb::string_t *dict_base) {
		if (current_dict != dict_base) {
			current_dict = dict_base;
			status.clear();
			keys.clear();
			groups.clear();
		}
	}

	const QualifyingGroup *Lookup(duckdb::idx_t dict_idx, const duckdb::string_t &adsh,
	                              const std::unordered_map<AdshKey, FilingInfo, AdshKeyHash> &map) {
		if (dict_idx >= status.size()) {
			status.resize(dict_idx + 1, AdshQualifyStatus::Unknown);
			keys.resize(dict_idx + 1);
			groups.resize(dict_idx + 1);
		}
		const auto adsh_key = AdshKey::From(adsh);
		auto &slot_status = status[dict_idx];
		if (slot_status != AdshQualifyStatus::Unknown) {
			if (keys[dict_idx] == adsh_key) {
				return slot_status == AdshQualifyStatus::Qualifies ? &groups[dict_idx] : nullptr;
			}
			slot_status = AdshQualifyStatus::Unknown;
		}
		keys[dict_idx] = adsh_key;
		const auto it = map.find(adsh_key);
		if (it == map.end()) {
			slot_status = AdshQualifyStatus::Rejects;
			return nullptr;
		}
		slot_status = AdshQualifyStatus::Qualifies;
		groups[dict_idx] = QualifyingGroup {it->second.group_id, it->second.cik};
		return &groups[dict_idx];
	}

	void PrewarmSelection(const ConsumableStringColumn &adsh_col, duckdb::idx_t count,
	                      const std::unordered_map<AdshKey, FilingInfo, AdshKeyHash> &map) {
		if (adsh_col.kind != ConsumableKind::Dict) {
			return;
		}
		BeginChunk(adsh_col.data);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto dict_idx = ConsumableRowIndex(adsh_col, row);
			if (dict_idx >= status.size() || status[dict_idx] == AdshQualifyStatus::Unknown) {
				Lookup(dict_idx, adsh_col.data[dict_idx], map);
			}
		}
	}

	const QualifyingGroup *LookupVerified(duckdb::idx_t dict_idx) const {
		if (dict_idx >= status.size()) {
			return nullptr;
		}
		if (status[dict_idx] == AdshQualifyStatus::Qualifies) {
			return &groups[dict_idx];
		}
		return nullptr;
	}
};

struct ProfileAggregateUs {
	SecEdgarQ3ProfileCounters *counters = nullptr;
	Clock::time_point start {};

	explicit ProfileAggregateUs(SecEdgarQ3ProfileCounters *profile_counters)
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

struct SecEdgarQ3Kernel::State {
	explicit State(duckdb::idx_t row_estimate) {
		const auto reserve_groups = row_estimate > 0 ? row_estimate / 100 : 4096;
		cik_totals.reserve(reserve_groups);
		groups.reserve(reserve_groups);
		group_sums.reserve(reserve_groups);
		name_cik_to_group.reserve(reserve_groups);
		fy2022_adsh_lookup.reserve(row_estimate > 0 ? row_estimate / 100 : 4096);
		num_buffer.reserve(row_estimate > 0 ? row_estimate / 4 : 65536);
		adsh_dict_cache.Reserve(row_estimate > 0 ? row_estimate / 50 : 8192);
	}

	std::unordered_map<AdshKey, FilingInfo, AdshKeyHash> fy2022_adsh_lookup;
	std::unordered_map<NameCikKey, uint32_t, NameCikKeyHash> name_cik_to_group;
	std::unordered_map<int32_t, double> cik_totals;
	std::vector<GroupMeta> groups;
	std::vector<double> group_sums;
	AdshDictQualifyingCache adsh_dict_cache;
	std::vector<ResultRow> ordered_output;
	std::vector<NumRowBuffer> num_buffer;
	bool fused_scans_done = false;
	bool sub_consumed = false;
	bool num_consumed = false;
	bool sub_fy_filter_pushed = false;
	bool num_uom_filter_pushed = false;
	bool finalized = false;
	duckdb::idx_t output_offset = 0;

	uint32_t InternGroup(const duckdb::string_t &name, int32_t cik) {
		const NameCikKey key {ToStoredString(name), cik};
		const auto it = name_cik_to_group.find(key);
		if (it != name_cik_to_group.end()) {
			return it->second;
		}
		const auto group_id = static_cast<uint32_t>(groups.size());
		groups.push_back(GroupMeta {key.name, cik, false});
		group_sums.push_back(0.0);
		name_cik_to_group.emplace(key, group_id);
		return group_id;
	}

	void InsertSubRow(const duckdb::string_t &adsh, const duckdb::string_t &name, int32_t cik, int32_t fy) {
		if (!sub_fy_filter_pushed && fy != kTargetFy) {
			return;
		}
		const auto adsh_key = AdshKey::From(adsh);
		if (fy2022_adsh_lookup.find(adsh_key) != fy2022_adsh_lookup.end()) {
			return;
		}
		const auto group_id = InternGroup(name, cik);
		fy2022_adsh_lookup.emplace(adsh_key, FilingInfo {group_id, cik});
	}

	void AccumulateValue(uint32_t group_id, int32_t cik, double value, SecEdgarQ3ProfileCounters *profile_counters) {
		auto &meta = groups[group_id];
		if (!meta.touched) {
			meta.touched = true;
			if (profile_counters) {
				profile_counters->distinct_inserts++;
			}
		}
		group_sums[group_id] += value;
		cik_totals[cik] += value;
		if (profile_counters) {
			profile_counters->qualifying_rows++;
		}
	}

	void ProcessNumRow(uint32_t group_id, int32_t cik, double value, SecEdgarQ3ProfileCounters *profile_counters) {
		AccumulateValue(group_id, cik, value, profile_counters);
	}

	const FilingInfo *LookupFy2022Flat(const duckdb::string_t &adsh) {
		const auto it = fy2022_adsh_lookup.find(AdshKey::From(adsh));
		if (it == fy2022_adsh_lookup.end()) {
			return nullptr;
		}
		return &it->second;
	}

	const QualifyingGroup *LookupFy2022Dict(duckdb::idx_t dict_idx, const duckdb::string_t &adsh,
	                                        const duckdb::string_t *dict_base) {
		adsh_dict_cache.BeginChunk(dict_base);
		return adsh_dict_cache.Lookup(dict_idx, adsh, fy2022_adsh_lookup);
	}

	void ProcessNumConsumableFiltered(duckdb::idx_t count, ConsumableStringColumn &adsh_col,
	                                  ConsumableDoubleColumn &value_col, ConsumableStringColumn &uom_col,
	                                  SecEdgarQ3ProfileCounters *profile_counters) {
		TrackChunkPath(profile_counters, count, ConsumableChunkPath(adsh_col, value_col));
		if (UomChunkNeverUsd(uom_col)) {
			return;
		}
		const bool uom_always_usd = num_uom_filter_pushed || UomChunkAlwaysUsd(uom_col);
		if (adsh_col.kind == ConsumableKind::Flat && value_col.kind == ConsumableKind::Flat && adsh_col.all_valid &&
		    value_col.all_valid) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto value_index = ConsumableRowIndex(value_col, row);
				const auto *filing = LookupFy2022Flat(adsh_col.data[adsh_index]);
				if (filing == nullptr) {
					continue;
				}
				ProcessNumRow(filing->group_id, filing->cik, value_col.data[value_index], profile_counters);
			}
			return;
		}
		const bool shared_sel = adsh_col.kind == ConsumableKind::Dict && value_col.kind == ConsumableKind::Dict &&
		                        adsh_col.sel == value_col.sel && adsh_col.all_valid && value_col.all_valid;
		if (shared_sel) {
			adsh_dict_cache.PrewarmSelection(adsh_col, count, fy2022_adsh_lookup);
			const auto &sel = *adsh_col.sel;
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto index = sel.get_index(row);
				const auto *qualifying = adsh_dict_cache.LookupVerified(index);
				if (qualifying == nullptr) {
					continue;
				}
				ProcessNumRow(qualifying->group_id, qualifying->cik, value_col.data[index], profile_counters);
			}
			return;
		}
		const bool adsh_dict_value_flat =
		    adsh_col.kind == ConsumableKind::Dict && value_col.kind == ConsumableKind::Flat && adsh_col.all_valid &&
		    value_col.all_valid;
		if (adsh_dict_value_flat) {
			adsh_dict_cache.PrewarmSelection(adsh_col, count, fy2022_adsh_lookup);
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto index = ConsumableRowIndex(adsh_col, row);
				const auto value_index = ConsumableRowIndex(value_col, row);
				const auto *qualifying = adsh_dict_cache.LookupVerified(index);
				if (qualifying == nullptr) {
					continue;
				}
				ProcessNumRow(qualifying->group_id, qualifying->cik, value_col.data[value_index], profile_counters);
			}
			return;
		}
		const bool value_dict_adsh_flat =
		    adsh_col.kind == ConsumableKind::Flat && value_col.kind == ConsumableKind::Dict && adsh_col.all_valid &&
		    value_col.all_valid;
		if (value_dict_adsh_flat) {
			for (duckdb::idx_t row = 0; row < count; row++) {
				if (!uom_always_usd && !UomQualifies(uom_col, row)) {
					continue;
				}
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto *filing = LookupFy2022Flat(adsh_col.data[adsh_index]);
				if (filing == nullptr) {
					continue;
				}
				const auto value_index = ConsumableRowIndex(value_col, row);
				ProcessNumRow(filing->group_id, filing->cik, value_col.data[value_index], profile_counters);
			}
			return;
		}
		if (adsh_col.kind == ConsumableKind::Dict) {
			adsh_dict_cache.BeginChunk(adsh_col.data);
		}
		for (duckdb::idx_t row = 0; row < count; row++) {
			if (!uom_always_usd && !UomQualifies(uom_col, row)) {
				continue;
			}
			const auto adsh_index = ConsumableRowIndex(adsh_col, row);
			const auto value_index = ConsumableRowIndex(value_col, row);
			if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(value_col, value_index)) {
				continue;
			}
			const auto &adsh = adsh_col.data[adsh_index];
			if (adsh_col.kind == ConsumableKind::Dict) {
				const auto *qualifying = adsh_dict_cache.Lookup(adsh_index, adsh, fy2022_adsh_lookup);
				if (qualifying == nullptr) {
					continue;
				}
				ProcessNumRow(qualifying->group_id, qualifying->cik, value_col.data[value_index], profile_counters);
			} else {
				const auto *filing = LookupFy2022Flat(adsh);
				if (filing == nullptr) {
					continue;
				}
				ProcessNumRow(filing->group_id, filing->cik, value_col.data[value_index], profile_counters);
			}
		}
	}

	void ProcessNumChunkVectors(duckdb::idx_t count, duckdb::Vector &adsh_vec, duckdb::Vector &value_vec,
	                            duckdb::Vector &uom_vec, SecEdgarQ3ProfileCounters *profile_counters) {
		ProfileAggregateUs aggregate_timer(profile_counters);
		ConsumableStringColumn adsh_col;
		ConsumableDoubleColumn value_col;
		ConsumableStringColumn uom_col;
		if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableDouble(value_vec, value_col)) {
			if (num_uom_filter_pushed) {
				uom_col.kind = ConsumableKind::Constant;
				uom_col.data = &UsdUomConstant();
				uom_col.all_valid = true;
				ProcessNumConsumableFiltered(count, adsh_col, value_col, uom_col, profile_counters);
				return;
			}
			if (BindConsumableString(uom_vec, uom_col)) {
				ProcessNumConsumableFiltered(count, adsh_col, value_col, uom_col, profile_counters);
				return;
			}
		}

		TrackChunkPath(profile_counters, count, 2);
		duckdb::UnifiedVectorFormat adsh_format;
		duckdb::UnifiedVectorFormat value_format;
		duckdb::UnifiedVectorFormat uom_format;
		adsh_vec.ToUnifiedFormat(count, adsh_format);
		value_vec.ToUnifiedFormat(count, value_format);
		uom_vec.ToUnifiedFormat(count, uom_format);
		auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
		auto value_values = duckdb::UnifiedVectorFormat::GetData<double>(value_format);
		auto uom_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(uom_format);
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto adsh_index = adsh_format.sel->get_index(row);
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
			const auto &adsh = adsh_values[adsh_index];
			const auto *filing = LookupFy2022Flat(adsh);
			if (filing == nullptr) {
				continue;
			}
			ProcessNumRow(filing->group_id, filing->cik, value_values[value_index], profile_counters);
		}
	}

	void ProcessNumRowStored(const std::string &adsh, double value, SecEdgarQ3ProfileCounters *profile_counters) {
		const auto it = fy2022_adsh_lookup.find(AdshKey::FromStored(adsh));
		if (it == fy2022_adsh_lookup.end()) {
			return;
		}
		ProcessNumRow(it->second.group_id, it->second.cik, value, profile_counters);
	}

	void ProcessBufferedNumRows(SecEdgarQ3ProfileCounters *profile_counters) {
		for (const auto &row : num_buffer) {
			ProcessNumRowStored(row.adsh, row.value, profile_counters);
		}
		num_buffer.clear();
	}
};

SecEdgarQ3Kernel::SecEdgarQ3Kernel(SecEdgarQ3Parameters params_p, SecEdgarQ3ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p), state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ3Kernel::~SecEdgarQ3Kernel() = default;

bool SecEdgarQ3Kernel::InputsConsumed() const {
	return state->fused_scans_done || (state->sub_consumed && state->num_consumed);
}

void SecEdgarQ3Kernel::MarkInputsConsumed() {
	state->fused_scans_done = true;
}

void SecEdgarQ3Kernel::ConsumeSubChunk(duckdb::DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	state->sub_consumed = true;
	auto &adsh_vec = input.data[SecEdgarQ3SubChunkLayout::ADSH_INDEX];
	auto &name_vec = input.data[SecEdgarQ3SubChunkLayout::NAME_INDEX];
	auto &cik_vec = input.data[SecEdgarQ3SubChunkLayout::CIK_INDEX];
	auto &fy_vec = input.data[SecEdgarQ3SubChunkLayout::FY_INDEX];

	ConsumableStringColumn adsh_col;
	ConsumableStringColumn name_col;
	ConsumableInt32Column cik_col;
	ConsumableInt32Column fy_col;
	if (BindConsumableString(adsh_vec, adsh_col) && BindConsumableString(name_vec, name_col) &&
	    BindConsumableInt32(cik_vec, cik_col) && BindConsumableInt32(fy_vec, fy_col)) {
		TrackChunkPath(profile, input.size(), ConsumableChunkPath(adsh_col, name_col, cik_col));
		if (adsh_col.kind == ConsumableKind::Flat && name_col.kind == ConsumableKind::Flat &&
		    cik_col.kind == ConsumableKind::Flat && fy_col.kind == ConsumableKind::Flat && adsh_col.all_valid &&
		    name_col.all_valid && cik_col.all_valid && fy_col.all_valid) {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				state->InsertSubRow(adsh_col.data[row], name_col.data[row], cik_col.data[row], fy_col.data[row]);
			}
		} else {
			for (duckdb::idx_t row = 0; row < input.size(); row++) {
				const auto adsh_index = ConsumableRowIndex(adsh_col, row);
				const auto name_index = ConsumableRowIndex(name_col, row);
				const auto cik_index = ConsumableRowIndex(cik_col, row);
				const auto fy_index = ConsumableRowIndex(fy_col, row);
				if (!ConsumableEntryValid(adsh_col, adsh_index) || !ConsumableEntryValid(name_col, name_index) ||
				    !ConsumableEntryValid(cik_col, cik_index) || !ConsumableEntryValid(fy_col, fy_index)) {
					continue;
				}
				state->InsertSubRow(adsh_col.data[adsh_index], name_col.data[name_index], cik_col.data[cik_index],
				                    fy_col.data[fy_index]);
			}
		}
		return;
	}

	TrackChunkPath(profile, input.size(), 2);
	duckdb::UnifiedVectorFormat adsh_format;
	duckdb::UnifiedVectorFormat name_format;
	duckdb::UnifiedVectorFormat cik_format;
	duckdb::UnifiedVectorFormat fy_format;
	adsh_vec.ToUnifiedFormat(input.size(), adsh_format);
	name_vec.ToUnifiedFormat(input.size(), name_format);
	cik_vec.ToUnifiedFormat(input.size(), cik_format);
	fy_vec.ToUnifiedFormat(input.size(), fy_format);
	auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
	auto name_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(name_format);
	auto cik_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(cik_format);
	auto fy_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(fy_format);
	for (duckdb::idx_t row = 0; row < input.size(); row++) {
		const auto adsh_index = adsh_format.sel->get_index(row);
		const auto name_index = name_format.sel->get_index(row);
		const auto cik_index = cik_format.sel->get_index(row);
		const auto fy_index = fy_format.sel->get_index(row);
		if (!adsh_format.validity.AllValid() && !adsh_format.validity.RowIsValidUnsafe(adsh_index)) {
			continue;
		}
		if (!name_format.validity.AllValid() && !name_format.validity.RowIsValidUnsafe(name_index)) {
			continue;
		}
		if (!cik_format.validity.AllValid() && !cik_format.validity.RowIsValidUnsafe(cik_index)) {
			continue;
		}
		if (!fy_format.validity.AllValid() && !fy_format.validity.RowIsValidUnsafe(fy_index)) {
			continue;
		}
		state->InsertSubRow(adsh_values[adsh_index], name_values[name_index], cik_values[cik_index],
		                    fy_values[fy_index]);
	}
}

void SecEdgarQ3Kernel::ConsumeNumChunk(duckdb::DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	state->num_consumed = true;
	auto &adsh_vec = input.data[SecEdgarQ3NumChunkLayout::ADSH_INDEX];
	auto &value_vec = input.data[SecEdgarQ3NumChunkLayout::VALUE_INDEX];
	auto &uom_vec = input.data[SecEdgarQ3NumChunkLayout::UOM_INDEX];

	const auto consume_start = Clock::now();
	state->ProcessNumChunkVectors(input.size(), adsh_vec, value_vec, uom_vec, profile);

	if (profile) {
		profile->pre_consume_us +=
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count();
	}
}

void SecEdgarQ3Kernel::ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) {
	if (input_id == static_cast<duckdb::idx_t>(SecEdgarQ3InputId::SUB)) {
		ConsumeSubChunk(input);
		return;
	}
	if (input_id == static_cast<duckdb::idx_t>(SecEdgarQ3InputId::NUM)) {
		ConsumeNumChunk(input);
		return;
	}
	throw duckdb::InternalException("SEC EDGAR Q3 received unexpected input id %llu", input_id);
}

void SecEdgarQ3Kernel::RunSubScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		ConsumeSubChunk(source->Chunk());
	}
	if (profile) {
		profile->sub_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void SecEdgarQ3Kernel::RunNumFusedPass(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	const auto scan_start = Clock::now();
	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		auto &chunk = source->Chunk();
		if (chunk.size() == 0) {
			continue;
		}
		auto &adsh_vec = chunk.data[SecEdgarQ3NumChunkLayout::ADSH_INDEX];
		auto &value_vec = chunk.data[SecEdgarQ3NumChunkLayout::VALUE_INDEX];
		auto &uom_vec = chunk.data[SecEdgarQ3NumChunkLayout::UOM_INDEX];
		state->ProcessNumChunkVectors(chunk.size(), adsh_vec, value_vec, uom_vec, profile);
	}
	if (profile) {
		profile->num_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
		duckdb::idx_t active = 0;
		for (const auto &meta : state->groups) {
			if (meta.touched) {
				active++;
			}
		}
		profile->active_groups = active;
	}
}

void SecEdgarQ3Kernel::FinalizeGroups() {
	if (state->finalized) {
		return;
	}

	if (!state->fused_scans_done && !state->num_buffer.empty()) {
		state->ProcessBufferedNumRows(profile);
	}

	double cik_sum_total = 0.0;
	for (const auto &entry : state->cik_totals) {
		cik_sum_total += entry.second;
	}
	const double avg_threshold =
	    state->cik_totals.empty() ? 0.0 : cik_sum_total / static_cast<double>(state->cik_totals.size());

	state->ordered_output.clear();
	state->ordered_output.reserve(state->groups.size());
	for (uint32_t group_id = 0; group_id < state->groups.size(); group_id++) {
		const auto total = state->group_sums[group_id];
		if (total <= avg_threshold) {
			continue;
		}
		const auto &meta = state->groups[group_id];
		state->ordered_output.push_back({meta.name, meta.cik, total});
	}

	std::sort(state->ordered_output.begin(), state->ordered_output.end(),
	          [](const ResultRow &lhs, const ResultRow &rhs) {
		          if (lhs.total_value != rhs.total_value) {
			          return lhs.total_value > rhs.total_value;
		          }
		          if (lhs.name != rhs.name) {
			          return lhs.name < rhs.name;
		          }
		          return lhs.cik < rhs.cik;
	          });
	if (state->ordered_output.size() > kResultLimit) {
		state->ordered_output.resize(kResultLimit);
	}

	if (profile) {
		profile->candidate_rows = state->ordered_output.size();
	}
	state->finalized = true;
}

void SecEdgarQ3Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeGroups();
	if (profile) {
		profile->finalize_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

SecEdgarQ3AdshKey ToAuxAdshKey(const AdshKey &key) {
	SecEdgarQ3AdshKey out;
	out.size = key.size;
	out.w0 = key.w0;
	out.w1 = key.w1;
	out.w2 = key.w2;
	out.w3 = 0;
	return out;
}

void SecEdgarQ3Kernel::ExportAuxPartitions(SecEdgarQ3Fy2022CohortPartition &cohort,
                                           SecEdgarQ3CikTotalsPartition &cik_totals,
                                           SecEdgarQ3OutputPartition &output) {
	if (!state->finalized) {
		FinalizeGroups();
	}

	cohort.adsh_to_filing.clear();
	cohort.adsh_to_filing.reserve(state->fy2022_adsh_lookup.size());
	for (const auto &entry : state->fy2022_adsh_lookup) {
		const auto &info = entry.second;
		const auto &meta = state->groups[info.group_id];
		cohort.adsh_to_filing.emplace(ToAuxAdshKey(entry.first),
		                            SecEdgarQ3FilingInfo {meta.name, meta.cik, info.group_id});
	}

	cik_totals.cik_totals = state->cik_totals;
	cik_totals.group_names.clear();
	cik_totals.group_ciks.clear();
	cik_totals.group_sums = state->group_sums;
	cik_totals.group_names.reserve(state->groups.size());
	cik_totals.group_ciks.reserve(state->groups.size());
	for (const auto &group : state->groups) {
		cik_totals.group_names.push_back(group.name);
		cik_totals.group_ciks.push_back(group.cik);
	}

	output.output_rows.clear();
	output.output_rows.reserve(state->ordered_output.size());
	for (const auto &row : state->ordered_output) {
		output.output_rows.push_back({row.name, row.cik, row.total_value});
	}
}

bool SecEdgarQ3Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_output.size()) {
		return false;
	}
	const auto remaining = state->ordered_output.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_output[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(result.name));
		output.SetValue(1, row, duckdb::Value::INTEGER(result.cik));
		output.SetValue(2, row, duckdb::Value::DOUBLE(result.total_value));
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3Kernel(const SecEdgarQ3Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ3Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3ProfilingKernel(const SecEdgarQ3Parameters &params,
                                                                    SecEdgarQ3ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ3Kernel>(params, &counters);
}

SecEdgarQ3FusedKernel::SecEdgarQ3FusedKernel(const SecEdgarQ3Parameters &params, SecEdgarQ3ProfileCounters *profile_p)
    : SecEdgarQ3Kernel(params, profile_p) {
}

void SecEdgarQ3FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q3 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ3FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &num_bind) {
	if (InputsConsumed()) {
		return;
	}

	const auto scan_start = Clock::now();
	const auto sub_bind = BindNativeScan(context, CreateSecEdgarQ3SubScanSpec());
	state->sub_fy_filter_pushed = NativeScanHasFilters(*sub_bind);
	state->num_uom_filter_pushed = NativeScanHasFilters(num_bind);
	// Fused prescan: sub builds adsh lookup, num aggregates USD values.
	RunSubScan(context, *sub_bind);
	state->adsh_dict_cache.Reserve(state->fy2022_adsh_lookup.size() * 2);
	RunNumFusedPass(context, num_bind);
	MarkInputsConsumed();

	if (profile) {
		profile->pre_scan_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
	}
}

void RunSecEdgarQ3FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &num_bind) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ3FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, num_bind);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3FusedKernel(const SecEdgarQ3Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ3FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ3FusedProfilingKernel(const SecEdgarQ3Parameters &params,
                                                                          SecEdgarQ3ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ3FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
