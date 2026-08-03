#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/sec_edgar_q1.hpp"
#include "accelerator_codegen/sec_edgar_q1_fused.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
constexpr duckdb::idx_t kFixedSlotCount = 5 * 256;
constexpr duckdb::idx_t kExpectedActiveGroups = 25;

enum class StmtSlot : uint8_t {
	BS = 0,
	IS = 1,
	CF = 2,
	EQ = 3,
	CI = 4,
	Unknown = 255,
};

struct GroupKey {
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = false;

	bool operator==(const GroupKey &other) const {
		return rfile_is_null == other.rfile_is_null && stmt == other.stmt && rfile == other.rfile;
	}
};

struct GroupKeyHash {
	size_t operator()(const GroupKey &key) const {
		return std::hash<std::string>()(key.stmt) ^ (std::hash<std::string>()(key.rfile) << 1) ^
		       (key.rfile_is_null ? 0x9e3779b9UL : 0UL);
	}
};

class DistinctAdshBitmap {
public:
	explicit DistinctAdshBitmap(duckdb::idx_t initial_words = 0) {
		if (initial_words > 0) {
			bits.assign(initial_words, 0);
		}
	}

	void Presize(duckdb::idx_t expected_ids) {
		const auto words = (expected_ids + 63) / 64;
		if (words > bits.size()) {
			bits.resize(words, 0);
		}
		presized_words = bits.size();
	}

	uint32_t size() const {
		return distinct_count;
	}

	bool insert_presized(uint32_t id) {
		const uint32_t word_idx = id >> 6;
		const uint64_t mask = uint64_t(1) << (id & 63);
		if (word_idx >= presized_words) {
			return insert(id);
		}
		auto &word = bits[word_idx];
		if (word & mask) {
			return false;
		}
		word |= mask;
		distinct_count++;
		return true;
	}

	bool insert(uint32_t id) {
		const uint32_t word_idx = id >> 6;
		const uint64_t mask = uint64_t(1) << (id & 63);
		if (word_idx >= bits.size()) {
			bits.resize(word_idx + 1, 0);
		}
		auto &word = bits[word_idx];
		if (word & mask) {
			return false;
		}
		word |= mask;
		distinct_count++;
		return true;
	}

private:
	std::vector<uint64_t> bits;
	duckdb::idx_t presized_words = 0;
	uint32_t distinct_count = 0;
};

struct GroupAggregate {
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = false;
	int64_t cnt = 0;
	int64_t line_sum = 0;
	int64_t line_count = 0;
	DistinctAdshBitmap distinct_adsh;
	uint32_t last_distinct_adsh = std::numeric_limits<uint32_t>::max();
	bool active = false;
};

struct OutputRow {
	std::string stmt;
	std::string rfile;
	bool rfile_is_null = false;
	int64_t cnt = 0;
	int64_t num_filings = 0;
	double avg_line_num = 0.0;
	bool has_avg = false;
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

constexpr duckdb::idx_t kMaxStmtDictEntries = 16;
constexpr duckdb::idx_t kMaxRfileDictEntries = 4;
constexpr duckdb::idx_t kMaxAdshDictCache = 2048;
constexpr uint16_t kSlotOverflow = std::numeric_limits<uint16_t>::max();

bool IsFlat(duckdb::Vector &vector) {
	return vector.GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
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

enum class ConsumableKind : uint8_t {
	Flat = 0,
	Dict = 1,
	Constant = 2,
};

struct ConsumableStringColumn {
	ConsumableKind kind = ConsumableKind::Flat;
	const duckdb::string_t *data = nullptr;
	const duckdb::SelectionVector *sel = nullptr;
	duckdb::ValidityMask validity {};
	bool all_valid = false;
};

struct ConsumableIntColumn {
	ConsumableKind kind = ConsumableKind::Flat;
	const int32_t *data = nullptr;
	const duckdb::SelectionVector *sel = nullptr;
	duckdb::ValidityMask validity {};
	bool all_valid = false;
};

bool IsConsumableVector(duckdb::Vector &vector) {
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		return true;
	case duckdb::VectorType::DICTIONARY_VECTOR:
		return duckdb::DictionaryVector::Child(vector).GetVectorType() == duckdb::VectorType::FLAT_VECTOR;
	case duckdb::VectorType::CONSTANT_VECTOR:
		return true;
	default:
		return false;
	}
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

bool BindConsumableInt(duckdb::Vector &vector, ConsumableIntColumn &out) {
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

duckdb::idx_t ConsumableRowIndex(const ConsumableIntColumn &column, duckdb::idx_t base) {
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

bool ConsumableEntryValid(const ConsumableIntColumn &column, duckdb::idx_t index) {
	return column.all_valid || column.validity.RowIsValidUnsafe(index);
}

bool AllConsumableVectors(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
                          duckdb::Vector &line_vec) {
	return IsConsumableVector(stmt_vec) && IsConsumableVector(rfile_vec) && IsConsumableVector(adsh_vec) &&
	       IsConsumableVector(line_vec);
}

void RecordLayout(SecEdgarQ1LayoutHistogram *histogram, duckdb::Vector &vector) {
	if (!histogram) {
		return;
	}
	switch (vector.GetVectorType()) {
	case duckdb::VectorType::FLAT_VECTOR:
		if (duckdb::FlatVector::Validity(vector).AllValid()) {
			histogram->flat_all_valid += 1;
		} else {
			histogram->flat_nullable += 1;
		}
		return;
	case duckdb::VectorType::DICTIONARY_VECTOR: {
		auto &child = duckdb::DictionaryVector::Child(vector);
		if (child.GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			if (duckdb::FlatVector::Validity(child).AllValid()) {
				histogram->dict_flat_all_valid += 1;
			} else {
				histogram->dict_flat_nullable += 1;
			}
		} else {
			histogram->other += 1;
		}
		return;
	}
	default:
		histogram->other += 1;
		return;
	}
}

void TrackPath(SecEdgarQ1ProfileCounters *profile, duckdb::idx_t rows, int path) {
	if (!profile) {
		return;
	}
	profile->pre_chunks += 1;
	profile->pre_rows += rows;
	switch (path) {
	case 0:
		profile->flat_chunks += 1;
		return;
	case 1:
		profile->dict_chunks += 1;
		return;
	default:
		profile->generic_chunks += 1;
		profile->generic_fallback_rows += rows;
		return;
	}
}

StmtSlot ClassifyStmt(const duckdb::string_t &value) {
	if (value.GetSize() != 2 || value.GetData() == nullptr) {
		return StmtSlot::Unknown;
	}
	const auto *data = value.GetData();
	if (data[0] == 'B' && data[1] == 'S') {
		return StmtSlot::BS;
	}
	if (data[0] == 'I' && data[1] == 'S') {
		return StmtSlot::IS;
	}
	if (data[0] == 'C' && data[1] == 'F') {
		return StmtSlot::CF;
	}
	if (data[0] == 'E' && data[1] == 'Q') {
		return StmtSlot::EQ;
	}
	if (data[0] == 'C' && data[1] == 'I') {
		return StmtSlot::CI;
	}
	return StmtSlot::Unknown;
}

uint16_t FixedSlotIndex(StmtSlot stmt, uint8_t rfile_char) {
	if (stmt == StmtSlot::Unknown) {
		return std::numeric_limits<uint16_t>::max();
	}
	return static_cast<uint16_t>(static_cast<uint8_t>(stmt) * 256U + rfile_char);
}

struct AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;
	uint64_t w3 = 0;

	static AdshKey From(const duckdb::string_t &adsh) {
		AdshKey key;
		key.size = static_cast<uint32_t>(adsh.GetSize());
		const auto *data = adsh.GetData();
		if (data == nullptr || key.size == 0) {
			return key;
		}
		const auto copy_word = [&](duckdb::idx_t offset, uint64_t &word) {
			if (key.size <= offset) {
				return;
			}
			std::memset(&word, 0, sizeof(word));
			std::memcpy(&word, data + offset, std::min<duckdb::idx_t>(key.size - offset, sizeof(word)));
		};
		copy_word(0, key.w0);
		copy_word(8, key.w1);
		copy_word(16, key.w2);
		copy_word(24, key.w3);
		return key;
	}

	bool operator==(const AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2 && w3 == other.w3;
	}
};

struct AdshKeyHash {
	size_t operator()(const AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ (key.w3 << 3) ^ key.size;
	}
};

class AdshInternTable {
public:
	explicit AdshInternTable(size_t reserve_hint) {
		lookup.reserve(reserve_hint);
	}

	uint32_t Intern(const duckdb::string_t &adsh) {
		const AdshKey key = AdshKey::From(adsh);
		const auto it = lookup.find(key);
		if (it != lookup.end()) {
			return it->second;
		}
		const uint32_t index = next_index++;
		lookup.emplace(key, index);
		return index;
	}

private:
	std::unordered_map<AdshKey, uint32_t, AdshKeyHash> lookup;
	uint32_t next_index = 0;
};

duckdb::idx_t SafeDictionarySize(duckdb::Vector &vector) {
	const auto dictionary_size = duckdb::DictionaryVector::DictionarySize(vector);
	if (dictionary_size.IsValid()) {
		return dictionary_size.GetIndex();
	}
	return 0;
}

bool DictionaryIndexInBounds(duckdb::Vector &vector, duckdb::idx_t index) {
	const auto dict_size = SafeDictionarySize(vector);
	return dict_size > 0 && index < dict_size;
}

bool ChunkDictionaryIndicesValid(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector *adsh_vec,
                                 duckdb::idx_t count, duckdb::SelectionVector *filter_sel) {
	const auto stmt_size = SafeDictionarySize(stmt_vec);
	const auto rfile_size = SafeDictionarySize(rfile_vec);
	const auto adsh_size = adsh_vec ? SafeDictionarySize(*adsh_vec) : 0;
	if (stmt_size == 0 || rfile_size == 0 || (adsh_vec && adsh_size == 0)) {
		return false;
	}
	const auto &stmt_sel = duckdb::DictionaryVector::SelVector(stmt_vec);
	const auto &rfile_sel = duckdb::DictionaryVector::SelVector(rfile_vec);
	const auto *adsh_sel = adsh_vec ? &duckdb::DictionaryVector::SelVector(*adsh_vec) : nullptr;
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		if (stmt_sel.get_index(base) >= stmt_size || rfile_sel.get_index(base) >= rfile_size) {
			return false;
		}
		if (adsh_sel && adsh_sel->get_index(base) >= adsh_size) {
			return false;
		}
	}
	return true;
}

struct DictSlotCaches {
	std::array<uint16_t, kMaxStmtDictEntries> stmt_base {};
	std::array<uint8_t, kMaxRfileDictEntries> rfile_char {};
	std::array<bool, kMaxRfileDictEntries> rfile_is_null {};
	std::array<uint16_t, kMaxStmtDictEntries * kMaxRfileDictEntries> slot_matrix {};

	void Precompute(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec) {
		stmt_base.fill(kSlotOverflow);
		rfile_is_null.fill(true);
		slot_matrix.fill(kSlotOverflow);

		auto &stmt_child = duckdb::DictionaryVector::Child(stmt_vec);
		auto &rfile_child = duckdb::DictionaryVector::Child(rfile_vec);
		const auto stmt_dict_size = std::min(SafeDictionarySize(stmt_vec), kMaxStmtDictEntries);
		const auto rfile_dict_size = std::min(SafeDictionarySize(rfile_vec), kMaxRfileDictEntries);
		const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_child);
		const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_child);
		auto &stmt_validity = duckdb::FlatVector::Validity(stmt_child);
		auto &rfile_validity = duckdb::FlatVector::Validity(rfile_child);

		for (duckdb::idx_t i = 0; i < stmt_dict_size; i++) {
			if (!stmt_validity.AllValid() && !stmt_validity.RowIsValidUnsafe(i)) {
				continue;
			}
			const auto stmt_slot = ClassifyStmt(stmt_data[i]);
			if (stmt_slot != StmtSlot::Unknown) {
				stmt_base[i] = static_cast<uint16_t>(static_cast<uint8_t>(stmt_slot) * 256U);
			}
		}

		for (duckdb::idx_t i = 0; i < rfile_dict_size; i++) {
			if (!rfile_validity.AllValid() && !rfile_validity.RowIsValidUnsafe(i)) {
				rfile_is_null[i] = true;
				continue;
			}
			const auto &rfile_value = rfile_data[i];
			if (rfile_value.GetSize() != 1 || rfile_value.GetData() == nullptr) {
				continue;
			}
			rfile_is_null[i] = false;
			rfile_char[i] = static_cast<uint8_t>(rfile_value.GetData()[0]);
		}

		for (duckdb::idx_t si = 0; si < stmt_dict_size; si++) {
			const auto base = stmt_base[si];
			if (base == kSlotOverflow) {
				continue;
			}
			for (duckdb::idx_t ri = 0; ri < rfile_dict_size; ri++) {
				if (rfile_is_null[ri]) {
					continue;
				}
				slot_matrix[si * kMaxRfileDictEntries + ri] = static_cast<uint16_t>(base + rfile_char[ri]);
			}
		}
	}

	uint16_t LookupFixedSlot(duckdb::idx_t stmt_index, duckdb::idx_t rfile_index) const {
		if (stmt_index >= kMaxStmtDictEntries || rfile_index >= kMaxRfileDictEntries) {
			return kSlotOverflow;
		}
		return slot_matrix[stmt_index * kMaxRfileDictEntries + rfile_index];
	}

	bool TryFixedSlot(duckdb::idx_t stmt_index, duckdb::idx_t rfile_index, uint16_t &slot_out) const {
		slot_out = LookupFixedSlot(stmt_index, rfile_index);
		return slot_out != kSlotOverflow;
	}
};

inline uint16_t LookupDictFixedSlot(const DictSlotCaches &caches, duckdb::idx_t stmt_index,
                                    duckdb::idx_t rfile_index) {
	if (stmt_index >= kMaxStmtDictEntries || rfile_index >= kMaxRfileDictEntries) {
		return kSlotOverflow;
	}
	return caches.slot_matrix[stmt_index * kMaxRfileDictEntries + rfile_index];
}

inline void AggregateFixedSlotHot(GroupAggregate &group, uint32_t adsh_intern, int32_t line,
                                  SecEdgarQ1ProfileCounters *prof) {
	group.cnt += 1;
	group.line_sum += line;
	group.line_count += 1;
	if (adsh_intern != group.last_distinct_adsh) {
		group.last_distinct_adsh = adsh_intern;
		if (group.distinct_adsh.insert_presized(adsh_intern) && prof) {
			prof->distinct_inserts += 1;
		}
	}
}

void DecodeFixedSlotLabel(uint16_t slot_index, std::string &stmt_out, std::string &rfile_out) {
	static constexpr const char *kStmtCodes[] = {"BS", "IS", "CF", "EQ", "CI"};
	const auto stmt_slot = slot_index / 256;
	const auto rfile_char = static_cast<char>(slot_index % 256);
	if (stmt_slot < 5) {
		stmt_out = kStmtCodes[stmt_slot];
	} else {
		stmt_out.clear();
	}
	rfile_out.assign(1, rfile_char);
}

} // namespace

struct SecEdgarQ1Kernel::State {
	std::array<GroupAggregate, kFixedSlotCount> fixed_slots {};
	std::unordered_map<GroupKey, GroupAggregate, GroupKeyHash> overflow_groups;
	std::vector<uint16_t> active_fixed_slots;
	std::vector<GroupKey> active_overflow_keys;
	AdshInternTable adsh_intern;
	const char *adsh_row_cache_data = nullptr;
	duckdb::idx_t adsh_row_cache_size = 0;
	uint32_t adsh_row_cache_intern = std::numeric_limits<uint32_t>::max();
	std::vector<OutputRow> ordered_rows;
	duckdb::idx_t output_offset = 0;
	bool finalized = false;
	bool input_consumed = false;
	duckdb::idx_t reserve_hint = 0;

	explicit State(duckdb::idx_t row_estimate)
	    : adsh_intern(row_estimate > 0 ? std::max<duckdb::idx_t>(row_estimate / 20, 4096) : 400000) {
		reserve_hint = row_estimate > 0 ? std::max<duckdb::idx_t>(row_estimate / 15, 8192) : 27000;
		active_fixed_slots.reserve(kExpectedActiveGroups);
		active_overflow_keys.reserve(8);
		ordered_rows.reserve(kExpectedActiveGroups);
	}

	uint32_t InternAdsh(const duckdb::string_t &adsh) {
		const auto size = adsh.GetSize();
		const auto *data = adsh.GetData();
		if (data == adsh_row_cache_data && size == adsh_row_cache_size &&
		    adsh_row_cache_intern != std::numeric_limits<uint32_t>::max()) {
			return adsh_row_cache_intern;
		}
		const auto interned = adsh_intern.Intern(adsh);
		adsh_row_cache_data = data;
		adsh_row_cache_size = size;
		adsh_row_cache_intern = interned;
		return interned;
	}

	void TouchFixedSlot(uint16_t slot_index) {
		auto &slot = fixed_slots[slot_index];
		if (slot.active) {
			return;
		}
		slot.active = true;
		slot.rfile_is_null = false;
		slot.distinct_adsh.Presize(reserve_hint);
		active_fixed_slots.push_back(slot_index);
	}

	void ActivateFixedSlot(uint16_t slot_index, const duckdb::string_t &stmt, const duckdb::string_t &rfile,
	                       bool rfile_is_null) {
		TouchFixedSlot(slot_index);
		auto &slot = fixed_slots[slot_index];
		slot.stmt = ToStoredString(stmt);
		slot.rfile = ToStoredString(rfile);
		slot.rfile_is_null = rfile_is_null;
	}

	GroupAggregate &ResolveFixed(uint16_t slot_index, const duckdb::string_t &stmt, const duckdb::string_t &rfile,
	                            bool rfile_is_null) {
		ActivateFixedSlot(slot_index, stmt, rfile, rfile_is_null);
		return fixed_slots[slot_index];
	}

	GroupAggregate &ResolveOverflow(const duckdb::string_t &stmt, const duckdb::string_t &rfile, bool rfile_is_null) {
		GroupKey key {ToStoredString(stmt), ToStoredString(rfile), rfile_is_null};
		auto it = overflow_groups.find(key);
		if (it == overflow_groups.end()) {
			GroupAggregate aggregate;
			aggregate.stmt = key.stmt;
			aggregate.rfile = key.rfile;
			aggregate.rfile_is_null = key.rfile_is_null;
			aggregate.active = true;
			aggregate.distinct_adsh.Presize(reserve_hint);
			it = overflow_groups.emplace(std::move(key), std::move(aggregate)).first;
			active_overflow_keys.push_back(it->first);
		}
		return it->second;
	}

	GroupAggregate &ResolveGroup(const duckdb::string_t &stmt, const duckdb::string_t &rfile, bool rfile_is_null) {
		if (rfile_is_null || rfile.GetSize() != 1 || rfile.GetData() == nullptr) {
			return ResolveOverflow(stmt, rfile, rfile_is_null);
		}
		const auto stmt_slot = ClassifyStmt(stmt);
		const auto slot_index = FixedSlotIndex(stmt_slot, static_cast<uint8_t>(rfile.GetData()[0]));
		if (slot_index == std::numeric_limits<uint16_t>::max()) {
			return ResolveOverflow(stmt, rfile, rfile_is_null);
		}
		return ResolveFixed(slot_index, stmt, rfile, rfile_is_null);
	}

	inline void InsertDistinct(GroupAggregate &group, uint32_t adsh_index, SecEdgarQ1ProfileCounters *profile_counters) {
		if (adsh_index == group.last_distinct_adsh) {
			return;
		}
		group.last_distinct_adsh = adsh_index;
		if (group.distinct_adsh.insert_presized(adsh_index) && profile_counters) {
			profile_counters->distinct_inserts += 1;
		}
	}

	void AggregateFixedRow(uint16_t slot_index, const duckdb::string_t &stmt, const duckdb::string_t &rfile,
	                       bool rfile_is_null, uint32_t adsh_index, int32_t line, SecEdgarQ1ProfileCounters *profile_counters) {
		auto &group = ResolveFixed(slot_index, stmt, rfile, rfile_is_null);
		group.cnt += 1;
		group.line_sum += line;
		group.line_count += 1;
		InsertDistinct(group, adsh_index, profile_counters);
	}

	void AggregateRow(GroupAggregate &group, const duckdb::string_t &adsh, bool adsh_valid, int32_t line, bool line_valid,
	                  SecEdgarQ1ProfileCounters *profile_counters) {
		group.cnt += 1;
		if (line_valid) {
			group.line_sum += line;
			group.line_count += 1;
		}
		if (adsh_valid) {
			InsertDistinct(group, InternAdsh(adsh), profile_counters);
		}
	}

	void AggregateRowIndex(GroupAggregate &group, uint32_t adsh_index, int32_t line, bool line_valid,
	                       SecEdgarQ1ProfileCounters *profile_counters) {
		group.cnt += 1;
		if (line_valid) {
			group.line_sum += line;
			group.line_count += 1;
		}
		InsertDistinct(group, adsh_index, profile_counters);
	}
};

SecEdgarQ1Kernel::SecEdgarQ1Kernel(SecEdgarQ1Parameters params_p, SecEdgarQ1ProfileCounters *profile_p)
    : params(std::move(params_p)), profile(profile_p),
      state(std::make_unique<State>(params.row_estimate)) {
}

SecEdgarQ1Kernel::~SecEdgarQ1Kernel() = default;

bool SecEdgarQ1Kernel::InputConsumed() const {
	return state->input_consumed;
}

void SecEdgarQ1Kernel::MarkInputConsumed() {
	state->input_consumed = true;
}

void SecEdgarQ1Kernel::ConsumeInput(duckdb::idx_t input_id, duckdb::DataChunk &input) {
	if (input_id != 0) {
		throw duckdb::InternalException("SEC EDGAR Q1 expects a single pre input");
	}
	ConsumePreChunk(input);
}

void SecEdgarQ1Kernel::ConsumePreChunk(duckdb::DataChunk &input) {
	if (input.size() == 0) {
		return;
	}
	state->input_consumed = true;
	auto &stmt = input.data[SecEdgarQ1PreChunkLayout::STMT_INDEX];
	auto &rfile = input.data[SecEdgarQ1PreChunkLayout::RFILE_INDEX];
	auto &adsh = input.data[SecEdgarQ1PreChunkLayout::ADSH_INDEX];
	auto &line = input.data[SecEdgarQ1PreChunkLayout::LINE_INDEX];
	ConsumePreVectors(stmt, rfile, adsh, line, input.size(), nullptr);
}

void SecEdgarQ1Kernel::ConsumePreVectors(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
                                         duckdb::Vector &line_vec, duckdb::idx_t count,
                                         duckdb::SelectionVector *filter_sel) {
	if (profile) {
		profile->qualifying_rows += count;
	}

	RecordLayout(profile ? &profile->layout_stmt : nullptr, stmt_vec);
	RecordLayout(profile ? &profile->layout_rfile : nullptr, rfile_vec);
	RecordLayout(profile ? &profile->layout_adsh : nullptr, adsh_vec);
	RecordLayout(profile ? &profile->layout_line : nullptr, line_vec);

	const auto consume_start = Clock::now();

	if (IsDictionaryOverFlat(stmt_vec) && IsDictionaryOverFlat(rfile_vec) && IsFlat(adsh_vec) && IsFlat(line_vec)) {
		TrackPath(profile, count, 1);
		ConsumePreDictDictFlatFlat(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	} else if (IsDictionaryOverFlat(stmt_vec) && IsDictionaryOverFlat(rfile_vec) && IsFlat(adsh_vec) &&
	           IsDictionaryOverFlat(line_vec)) {
		TrackPath(profile, count, 1);
		ConsumePreDictDictFlatDict(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	} else if (IsDictionaryOverFlat(stmt_vec) && IsDictionaryOverFlat(rfile_vec) && IsDictionaryOverFlat(adsh_vec) &&
	           IsFlat(line_vec)) {
		TrackPath(profile, count, 1);
		ConsumePreDictDictDictFlat(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	} else if (IsDictionaryOverFlat(stmt_vec) && IsDictionaryOverFlat(rfile_vec) && IsDictionaryOverFlat(adsh_vec) &&
	           IsDictionaryOverFlat(line_vec)) {
		TrackPath(profile, count, 1);
		ConsumePreDictDictDictDict(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	} else if (IsFlat(stmt_vec) && IsFlat(rfile_vec) && IsFlat(adsh_vec) && IsFlat(line_vec)) {
		if (IsFlatAllValid(stmt_vec) && IsFlatAllValid(rfile_vec) && IsFlatAllValid(adsh_vec) &&
		    IsFlatAllValid(line_vec)) {
			TrackPath(profile, count, 0);
			ConsumePreFlatFlatFlatFlat(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
		} else {
			TrackPath(profile, count, 0);
			ConsumePreFlatNullable(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
		}
	} else if (AllConsumableVectors(stmt_vec, rfile_vec, adsh_vec, line_vec)) {
		TrackPath(profile, count, 1);
		ConsumePreConsumableMixed(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	} else {
		TrackPath(profile, count, 2);
		ConsumePreGeneric(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
	}

	if (profile) {
		profile->pre_consume_us +=
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - consume_start).count();
	}
}

void SecEdgarQ1Kernel::ConsumePreDictDictFlatFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                 duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                 duckdb::SelectionVector *filter_sel) {
	auto &stmt_child = duckdb::DictionaryVector::Child(stmt_vec);
	auto &rfile_child = duckdb::DictionaryVector::Child(rfile_vec);
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_child);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_child);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_vec);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_vec);
	const auto &stmt_sel = duckdb::DictionaryVector::SelVector(stmt_vec);
	const auto &rfile_sel = duckdb::DictionaryVector::SelVector(rfile_vec);
	auto &stmt_validity = duckdb::FlatVector::Validity(stmt_child);
	auto &rfile_validity = duckdb::FlatVector::Validity(rfile_child);
	auto &adsh_validity = duckdb::FlatVector::Validity(adsh_vec);
	auto &line_validity = duckdb::FlatVector::Validity(line_vec);

	DictSlotCaches slot_caches;
	slot_caches.Precompute(stmt_vec, rfile_vec);

	const bool stmt_all_valid = stmt_validity.AllValid();
	const bool rfile_all_valid = rfile_validity.AllValid();
	const bool adsh_all_valid = adsh_validity.AllValid();
	const bool line_all_valid = line_validity.AllValid();

	if (stmt_all_valid && rfile_all_valid && adsh_all_valid && line_all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base = filter_sel ? filter_sel->get_index(row) : row;
			const auto stmt_index = stmt_sel.get_index(base);
			const auto rfile_index = rfile_sel.get_index(base);
			const uint16_t slot_index = LookupDictFixedSlot(slot_caches, stmt_index, rfile_index);
			if (slot_index != kSlotOverflow) {
				const auto adsh_intern = state->InternAdsh(adsh_data[base]);
				auto &group = state->fixed_slots[slot_index];
				if (!group.active) {
					state->TouchFixedSlot(slot_index);
				}
				AggregateFixedSlotHot(group, adsh_intern, line_data[base], profile);
				continue;
			}
			const bool rfile_is_null =
			    rfile_index < kMaxRfileDictEntries && slot_caches.rfile_is_null[rfile_index];
			auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], rfile_is_null);
			state->AggregateRowIndex(group, state->InternAdsh(adsh_data[base]), line_data[base], true, profile);
		}
		return;
	}

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = stmt_sel.get_index(base);
		const auto rfile_index = rfile_sel.get_index(base);
		if (!stmt_all_valid && !stmt_validity.RowIsValidUnsafe(stmt_index)) {
			continue;
		}
		const bool rfile_is_null =
		    !rfile_all_valid && !rfile_validity.RowIsValidUnsafe(rfile_index);
		const bool adsh_valid = adsh_all_valid || adsh_validity.RowIsValidUnsafe(base);
		const bool line_valid = line_all_valid || line_validity.RowIsValidUnsafe(base);

		uint16_t slot_index = kSlotOverflow;
		if (!rfile_is_null && slot_caches.TryFixedSlot(stmt_index, rfile_index, slot_index)) {
			uint32_t adsh_index = std::numeric_limits<uint32_t>::max();
			if (adsh_valid) {
				adsh_index = state->InternAdsh(adsh_data[base]);
			}
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_data[stmt_index], rfile_data[rfile_index], false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[base];
				group.line_count += 1;
			}
			if (adsh_valid) {
				state->InsertDistinct(group, adsh_index, profile);
			}
			continue;
		}
		auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], rfile_is_null);
		state->AggregateRow(group, adsh_data[base], adsh_valid, line_data[base], line_valid, profile);
	}
}

void SecEdgarQ1Kernel::ConsumePreDictDictDictFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                 duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                 duckdb::SelectionVector *filter_sel) {
	auto &stmt_child = duckdb::DictionaryVector::Child(stmt_vec);
	auto &rfile_child = duckdb::DictionaryVector::Child(rfile_vec);
	auto &adsh_child = duckdb::DictionaryVector::Child(adsh_vec);
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_child);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_child);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_child);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_vec);
	const auto &stmt_sel = duckdb::DictionaryVector::SelVector(stmt_vec);
	const auto &rfile_sel = duckdb::DictionaryVector::SelVector(rfile_vec);
	const auto &adsh_sel = duckdb::DictionaryVector::SelVector(adsh_vec);
	auto &stmt_validity = duckdb::FlatVector::Validity(stmt_child);
	auto &rfile_validity = duckdb::FlatVector::Validity(rfile_child);
	auto &adsh_validity = duckdb::FlatVector::Validity(adsh_child);
	auto &line_validity = duckdb::FlatVector::Validity(line_vec);

	DictSlotCaches slot_caches;
	slot_caches.Precompute(stmt_vec, rfile_vec);

	const auto adsh_dict_size = std::min(SafeDictionarySize(adsh_vec), kMaxAdshDictCache);
	std::array<uint32_t, kMaxAdshDictCache> adsh_intern_cache {};
	std::array<bool, kMaxAdshDictCache> adsh_cache_valid {};

	const bool stmt_all_valid = stmt_validity.AllValid();
	const bool rfile_all_valid = rfile_validity.AllValid();
	const bool adsh_all_valid = adsh_validity.AllValid();
	const bool line_all_valid = line_validity.AllValid();

	if (stmt_all_valid && rfile_all_valid && adsh_all_valid && line_all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base = filter_sel ? filter_sel->get_index(row) : row;
			const auto stmt_index = stmt_sel.get_index(base);
			const auto rfile_index = rfile_sel.get_index(base);
			const auto adsh_index = adsh_sel.get_index(base);

			uint32_t interned = std::numeric_limits<uint32_t>::max();
			if (adsh_index < adsh_dict_size && adsh_cache_valid[adsh_index]) {
				interned = adsh_intern_cache[adsh_index];
			} else {
				interned = state->InternAdsh(adsh_data[adsh_index]);
				if (adsh_index < adsh_dict_size) {
					adsh_intern_cache[adsh_index] = interned;
					adsh_cache_valid[adsh_index] = true;
				}
			}

			const uint16_t slot_index = LookupDictFixedSlot(slot_caches, stmt_index, rfile_index);
			if (slot_index != kSlotOverflow) {
				auto &group = state->fixed_slots[slot_index];
				if (!group.active) {
					state->TouchFixedSlot(slot_index);
				}
				AggregateFixedSlotHot(group, interned, line_data[base], profile);
				continue;
			}

			auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], false);
			state->AggregateRowIndex(group, interned, line_data[base], true, profile);
		}
		return;
	}

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = stmt_sel.get_index(base);
		const auto rfile_index = rfile_sel.get_index(base);
		const auto adsh_index = adsh_sel.get_index(base);
		if (!stmt_all_valid && !stmt_validity.RowIsValidUnsafe(stmt_index)) {
			continue;
		}
		const bool rfile_is_null =
		    !rfile_all_valid && !rfile_validity.RowIsValidUnsafe(rfile_index);
		const bool line_valid = line_all_valid || line_validity.RowIsValidUnsafe(base);

		uint32_t interned = std::numeric_limits<uint32_t>::max();
		if (adsh_all_valid || adsh_validity.RowIsValidUnsafe(adsh_index)) {
			if (adsh_index < adsh_dict_size && adsh_cache_valid[adsh_index]) {
				interned = adsh_intern_cache[adsh_index];
			} else {
				interned = state->InternAdsh(adsh_data[adsh_index]);
				if (adsh_index < adsh_dict_size) {
					adsh_intern_cache[adsh_index] = interned;
					adsh_cache_valid[adsh_index] = true;
				}
			}
		}

		uint16_t slot_index = kSlotOverflow;
		if (!rfile_is_null && slot_caches.TryFixedSlot(stmt_index, rfile_index, slot_index)) {
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_data[stmt_index], rfile_data[rfile_index], false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[base];
				group.line_count += 1;
			}
			if (interned != std::numeric_limits<uint32_t>::max()) {
				state->InsertDistinct(group, interned, profile);
			}
			continue;
		}

		auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], rfile_is_null);
		if (interned == std::numeric_limits<uint32_t>::max()) {
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[base];
				group.line_count += 1;
			}
		} else {
			state->AggregateRowIndex(group, interned, line_data[base], line_valid, profile);
		}
	}
}

void SecEdgarQ1Kernel::ConsumePreDictDictFlatDict(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                 duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                 duckdb::SelectionVector *filter_sel) {
	auto &stmt_child = duckdb::DictionaryVector::Child(stmt_vec);
	auto &rfile_child = duckdb::DictionaryVector::Child(rfile_vec);
	auto &line_child = duckdb::DictionaryVector::Child(line_vec);
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_child);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_child);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_vec);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_child);
	const auto &stmt_sel = duckdb::DictionaryVector::SelVector(stmt_vec);
	const auto &rfile_sel = duckdb::DictionaryVector::SelVector(rfile_vec);
	const auto &line_sel = duckdb::DictionaryVector::SelVector(line_vec);
	auto &stmt_validity = duckdb::FlatVector::Validity(stmt_child);
	auto &rfile_validity = duckdb::FlatVector::Validity(rfile_child);
	auto &adsh_validity = duckdb::FlatVector::Validity(adsh_vec);
	auto &line_validity = duckdb::FlatVector::Validity(line_child);

	DictSlotCaches slot_caches;
	slot_caches.Precompute(stmt_vec, rfile_vec);

	const bool stmt_all_valid = stmt_validity.AllValid();
	const bool rfile_all_valid = rfile_validity.AllValid();
	const bool adsh_all_valid = adsh_validity.AllValid();
	const bool line_all_valid = line_validity.AllValid();

	if (stmt_all_valid && rfile_all_valid && adsh_all_valid && line_all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base = filter_sel ? filter_sel->get_index(row) : row;
			const auto stmt_index = stmt_sel.get_index(base);
			const auto rfile_index = rfile_sel.get_index(base);
			const auto line_index = line_sel.get_index(base);
			const auto adsh_index = state->InternAdsh(adsh_data[base]);

			const uint16_t slot_index = LookupDictFixedSlot(slot_caches, stmt_index, rfile_index);
			if (slot_index != kSlotOverflow) {
				auto &group = state->fixed_slots[slot_index];
				if (!group.active) {
					state->TouchFixedSlot(slot_index);
				}
				AggregateFixedSlotHot(group, adsh_index, line_data[line_index], profile);
				continue;
			}
			auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], false);
			state->AggregateRowIndex(group, adsh_index, line_data[line_index], true, profile);
		}
		return;
	}

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = stmt_sel.get_index(base);
		const auto rfile_index = rfile_sel.get_index(base);
		const auto line_index = line_sel.get_index(base);
		if (!stmt_all_valid && !stmt_validity.RowIsValidUnsafe(stmt_index)) {
			continue;
		}
		const bool rfile_is_null = !rfile_all_valid && !rfile_validity.RowIsValidUnsafe(rfile_index);
		const bool adsh_valid = adsh_all_valid || adsh_validity.RowIsValidUnsafe(base);
		const bool line_valid = line_all_valid || line_validity.RowIsValidUnsafe(line_index);

		uint16_t slot_index = kSlotOverflow;
		if (!rfile_is_null && slot_caches.TryFixedSlot(stmt_index, rfile_index, slot_index)) {
			uint32_t adsh_index = std::numeric_limits<uint32_t>::max();
			if (adsh_valid) {
				adsh_index = state->InternAdsh(adsh_data[base]);
			}
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_data[stmt_index], rfile_data[rfile_index], false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[line_index];
				group.line_count += 1;
			}
			if (adsh_valid) {
				state->InsertDistinct(group, adsh_index, profile);
			}
			continue;
		}
		auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], rfile_is_null);
		state->AggregateRow(group, adsh_data[base], adsh_valid, line_valid ? line_data[line_index] : 0, line_valid,
		                    profile);
	}
}

void SecEdgarQ1Kernel::ConsumePreDictDictDictDict(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                  duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                  duckdb::SelectionVector *filter_sel) {
	auto &stmt_child = duckdb::DictionaryVector::Child(stmt_vec);
	auto &rfile_child = duckdb::DictionaryVector::Child(rfile_vec);
	auto &adsh_child = duckdb::DictionaryVector::Child(adsh_vec);
	auto &line_child = duckdb::DictionaryVector::Child(line_vec);
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_child);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_child);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_child);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_child);
	const auto &stmt_sel = duckdb::DictionaryVector::SelVector(stmt_vec);
	const auto &rfile_sel = duckdb::DictionaryVector::SelVector(rfile_vec);
	const auto &adsh_sel = duckdb::DictionaryVector::SelVector(adsh_vec);
	const auto &line_sel = duckdb::DictionaryVector::SelVector(line_vec);
	auto &stmt_validity = duckdb::FlatVector::Validity(stmt_child);
	auto &rfile_validity = duckdb::FlatVector::Validity(rfile_child);
	auto &adsh_validity = duckdb::FlatVector::Validity(adsh_child);
	auto &line_validity = duckdb::FlatVector::Validity(line_child);

	DictSlotCaches slot_caches;
	slot_caches.Precompute(stmt_vec, rfile_vec);

	const auto adsh_dict_size = std::min(SafeDictionarySize(adsh_vec), kMaxAdshDictCache);
	std::array<uint32_t, kMaxAdshDictCache> adsh_intern_cache {};
	std::array<bool, kMaxAdshDictCache> adsh_cache_valid {};

	const bool stmt_all_valid = stmt_validity.AllValid();
	const bool rfile_all_valid = rfile_validity.AllValid();
	const bool adsh_all_valid = adsh_validity.AllValid();
	const bool line_all_valid = line_validity.AllValid();

	if (stmt_all_valid && rfile_all_valid && adsh_all_valid && line_all_valid) {
		for (duckdb::idx_t row = 0; row < count; row++) {
			const auto base = filter_sel ? filter_sel->get_index(row) : row;
			const auto stmt_index = stmt_sel.get_index(base);
			const auto rfile_index = rfile_sel.get_index(base);
			const auto adsh_index = adsh_sel.get_index(base);
			const auto line_index = line_sel.get_index(base);

			uint32_t interned = std::numeric_limits<uint32_t>::max();
			if (adsh_index < adsh_dict_size && adsh_cache_valid[adsh_index]) {
				interned = adsh_intern_cache[adsh_index];
			} else {
				interned = state->InternAdsh(adsh_data[adsh_index]);
				if (adsh_index < adsh_dict_size) {
					adsh_intern_cache[adsh_index] = interned;
					adsh_cache_valid[adsh_index] = true;
				}
			}

			const uint16_t slot_index = LookupDictFixedSlot(slot_caches, stmt_index, rfile_index);
			if (slot_index != kSlotOverflow) {
				auto &group = state->fixed_slots[slot_index];
				if (!group.active) {
					state->TouchFixedSlot(slot_index);
				}
				AggregateFixedSlotHot(group, interned, line_data[line_index], profile);
				continue;
			}

			auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], false);
			state->AggregateRowIndex(group, interned, line_data[line_index], true, profile);
		}
		return;
	}

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = stmt_sel.get_index(base);
		const auto rfile_index = rfile_sel.get_index(base);
		const auto adsh_index = adsh_sel.get_index(base);
		const auto line_index = line_sel.get_index(base);
		if (!stmt_all_valid && !stmt_validity.RowIsValidUnsafe(stmt_index)) {
			continue;
		}
		const bool rfile_is_null = !rfile_all_valid && !rfile_validity.RowIsValidUnsafe(rfile_index);
		const bool line_valid = line_all_valid || line_validity.RowIsValidUnsafe(line_index);

		uint32_t interned = std::numeric_limits<uint32_t>::max();
		if (adsh_all_valid || adsh_validity.RowIsValidUnsafe(adsh_index)) {
			if (adsh_index < adsh_dict_size && adsh_cache_valid[adsh_index]) {
				interned = adsh_intern_cache[adsh_index];
			} else {
				interned = state->InternAdsh(adsh_data[adsh_index]);
				if (adsh_index < adsh_dict_size) {
					adsh_intern_cache[adsh_index] = interned;
					adsh_cache_valid[adsh_index] = true;
				}
			}
		}

		uint16_t slot_index = kSlotOverflow;
		if (!rfile_is_null && slot_caches.TryFixedSlot(stmt_index, rfile_index, slot_index)) {
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_data[stmt_index], rfile_data[rfile_index], false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[line_index];
				group.line_count += 1;
			}
			if (interned != std::numeric_limits<uint32_t>::max()) {
				state->InsertDistinct(group, interned, profile);
			}
			continue;
		}

		auto &group = state->ResolveGroup(stmt_data[stmt_index], rfile_data[rfile_index], rfile_is_null);
		if (interned == std::numeric_limits<uint32_t>::max()) {
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[line_index];
				group.line_count += 1;
			}
		} else {
			state->AggregateRowIndex(group, interned, line_valid ? line_data[line_index] : 0, line_valid, profile);
		}
	}
}

void SecEdgarQ1Kernel::ConsumePreFlatFlatFlatFlat(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                  duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                  duckdb::SelectionVector *filter_sel) {
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_vec);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_vec);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_vec);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_vec);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_slot = ClassifyStmt(stmt_data[base]);
		const auto &rfile_value = rfile_data[base];
		if (stmt_slot == StmtSlot::Unknown || rfile_value.GetSize() != 1 || rfile_value.GetData() == nullptr) {
			auto &group = state->ResolveGroup(stmt_data[base], rfile_value, false);
			state->AggregateRow(group, adsh_data[base], true, line_data[base], true, profile);
			continue;
		}
		const auto slot_index =
		    FixedSlotIndex(stmt_slot, static_cast<uint8_t>(rfile_value.GetData()[0]));
		const auto adsh_intern = state->InternAdsh(adsh_data[base]);
		auto &group = state->fixed_slots[slot_index];
		if (!group.active) {
			state->TouchFixedSlot(slot_index);
		}
		AggregateFixedSlotHot(group, adsh_intern, line_data[base], profile);
	}
}

void SecEdgarQ1Kernel::ConsumePreFlatNullable(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                              duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                              duckdb::SelectionVector *filter_sel) {
	const auto *stmt_data = duckdb::FlatVector::GetData<duckdb::string_t>(stmt_vec);
	const auto *rfile_data = duckdb::FlatVector::GetData<duckdb::string_t>(rfile_vec);
	const auto *adsh_data = duckdb::FlatVector::GetData<duckdb::string_t>(adsh_vec);
	const auto *line_data = duckdb::FlatVector::GetData<int32_t>(line_vec);
	auto &stmt_validity = duckdb::FlatVector::Validity(stmt_vec);
	auto &rfile_validity = duckdb::FlatVector::Validity(rfile_vec);
	auto &adsh_validity = duckdb::FlatVector::Validity(adsh_vec);
	auto &line_validity = duckdb::FlatVector::Validity(line_vec);

	const bool stmt_all_valid = stmt_validity.AllValid();
	const bool rfile_all_valid = rfile_validity.AllValid();
	const bool adsh_all_valid = adsh_validity.AllValid();
	const bool line_all_valid = line_validity.AllValid();

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		if (!stmt_all_valid && !stmt_validity.RowIsValidUnsafe(base)) {
			continue;
		}
		const bool rfile_is_null = !rfile_all_valid && !rfile_validity.RowIsValidUnsafe(base);
		const bool adsh_valid = adsh_all_valid || adsh_validity.RowIsValidUnsafe(base);
		const bool line_valid = line_all_valid || line_validity.RowIsValidUnsafe(base);

		const auto stmt_slot = ClassifyStmt(stmt_data[base]);
		const duckdb::string_t &rfile_value = rfile_data[base];
		if (!rfile_is_null && stmt_slot != StmtSlot::Unknown && rfile_value.GetSize() == 1 &&
		    rfile_value.GetData() != nullptr) {
			const auto slot_index =
			    FixedSlotIndex(stmt_slot, static_cast<uint8_t>(rfile_value.GetData()[0]));
			uint32_t adsh_index = std::numeric_limits<uint32_t>::max();
			if (adsh_valid) {
				adsh_index = state->InternAdsh(adsh_data[base]);
			}
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_data[base], rfile_value, false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_data[base];
				group.line_count += 1;
			}
			if (adsh_valid) {
				state->InsertDistinct(group, adsh_index, profile);
			}
			continue;
		}
		auto &group = state->ResolveGroup(stmt_data[base], rfile_value, rfile_is_null);
		state->AggregateRow(group, adsh_data[base], adsh_valid, line_data[base], line_valid, profile);
	}
}

void SecEdgarQ1Kernel::ConsumePreConsumableMixed(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec,
                                                 duckdb::Vector &adsh_vec, duckdb::Vector &line_vec, duckdb::idx_t count,
                                                 duckdb::SelectionVector *filter_sel) {
	ConsumableStringColumn stmt_col;
	ConsumableStringColumn rfile_col;
	ConsumableStringColumn adsh_col;
	ConsumableIntColumn line_col;
	if (!BindConsumableString(stmt_vec, stmt_col) || !BindConsumableString(rfile_vec, rfile_col) ||
	    !BindConsumableString(adsh_vec, adsh_col) || !BindConsumableInt(line_vec, line_col)) {
		ConsumePreGeneric(stmt_vec, rfile_vec, adsh_vec, line_vec, count, filter_sel);
		return;
	}

	std::optional<DictSlotCaches> slot_caches;
	if (stmt_col.kind == ConsumableKind::Dict && rfile_col.kind == ConsumableKind::Dict) {
		slot_caches.emplace();
		slot_caches->Precompute(stmt_vec, rfile_vec);
	}

	std::array<uint32_t, kMaxAdshDictCache> adsh_intern_cache {};
	std::array<bool, kMaxAdshDictCache> adsh_cache_valid {};
	const auto adsh_dict_size =
	    adsh_col.kind == ConsumableKind::Dict ? std::min(SafeDictionarySize(adsh_vec), kMaxAdshDictCache) : 0;

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = ConsumableRowIndex(stmt_col, base);
		const auto rfile_index = ConsumableRowIndex(rfile_col, base);
		const auto adsh_index = ConsumableRowIndex(adsh_col, base);
		const auto line_index = ConsumableRowIndex(line_col, base);
		if (!ConsumableEntryValid(stmt_col, stmt_index)) {
			continue;
		}
		const bool rfile_is_null = !ConsumableEntryValid(rfile_col, rfile_index);
		const bool adsh_valid = ConsumableEntryValid(adsh_col, adsh_index);
		const bool line_valid = ConsumableEntryValid(line_col, line_index);
		const duckdb::string_t &stmt_value = stmt_col.data[stmt_index];
		const duckdb::string_t &rfile_value = rfile_col.data[rfile_index];

		uint32_t interned = std::numeric_limits<uint32_t>::max();
		if (adsh_valid) {
			if (adsh_col.kind == ConsumableKind::Dict && adsh_index < adsh_dict_size && adsh_cache_valid[adsh_index]) {
				interned = adsh_intern_cache[adsh_index];
			} else {
				interned = state->InternAdsh(adsh_col.data[adsh_index]);
				if (adsh_col.kind == ConsumableKind::Dict && adsh_index < adsh_dict_size) {
					adsh_intern_cache[adsh_index] = interned;
					adsh_cache_valid[adsh_index] = true;
				}
			}
		}

		uint16_t slot_index = kSlotOverflow;
		if (!rfile_is_null && slot_caches.has_value() &&
		    slot_caches->TryFixedSlot(stmt_index, rfile_index, slot_index)) {
			auto &group = state->fixed_slots[slot_index];
			if (!group.active) {
				state->ActivateFixedSlot(slot_index, stmt_value, rfile_value, false);
			}
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_col.data[line_index];
				group.line_count += 1;
			}
			if (interned != std::numeric_limits<uint32_t>::max()) {
				state->InsertDistinct(group, interned, profile);
			}
			continue;
		}

		if (!rfile_is_null) {
			const auto stmt_slot = ClassifyStmt(stmt_value);
			if (stmt_slot != StmtSlot::Unknown && rfile_value.GetSize() == 1 && rfile_value.GetData() != nullptr) {
				slot_index = FixedSlotIndex(stmt_slot, static_cast<uint8_t>(rfile_value.GetData()[0]));
				if (slot_index != kSlotOverflow) {
					auto &group = state->fixed_slots[slot_index];
					if (!group.active) {
						state->ActivateFixedSlot(slot_index, stmt_value, rfile_value, false);
					}
					group.cnt += 1;
					if (line_valid) {
						group.line_sum += line_col.data[line_index];
						group.line_count += 1;
					}
					if (interned != std::numeric_limits<uint32_t>::max()) {
						state->InsertDistinct(group, interned, profile);
					}
					continue;
				}
			}
		}

		auto &group = state->ResolveGroup(stmt_value, rfile_value, rfile_is_null);
		if (interned == std::numeric_limits<uint32_t>::max()) {
			group.cnt += 1;
			if (line_valid) {
				group.line_sum += line_col.data[line_index];
				group.line_count += 1;
			}
		} else {
			state->AggregateRowIndex(group, interned, line_valid ? line_col.data[line_index] : 0, line_valid, profile);
		}
	}
}

void SecEdgarQ1Kernel::ConsumePreGeneric(duckdb::Vector &stmt_vec, duckdb::Vector &rfile_vec, duckdb::Vector &adsh_vec,
                                         duckdb::Vector &line_vec, duckdb::idx_t count,
                                         duckdb::SelectionVector *filter_sel) {
	const duckdb::idx_t scan_count = filter_sel ? count : count;
	duckdb::UnifiedVectorFormat stmt_format;
	duckdb::UnifiedVectorFormat rfile_format;
	duckdb::UnifiedVectorFormat adsh_format;
	duckdb::UnifiedVectorFormat line_format;
	stmt_vec.ToUnifiedFormat(scan_count, stmt_format);
	rfile_vec.ToUnifiedFormat(scan_count, rfile_format);
	adsh_vec.ToUnifiedFormat(scan_count, adsh_format);
	line_vec.ToUnifiedFormat(scan_count, line_format);

	auto stmt_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(stmt_format);
	auto rfile_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(rfile_format);
	auto adsh_values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(adsh_format);
	auto line_values = duckdb::UnifiedVectorFormat::GetData<int32_t>(line_format);

	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto base = filter_sel ? filter_sel->get_index(row) : row;
		const auto stmt_index = stmt_format.sel->get_index(base);
		const auto rfile_index = rfile_format.sel->get_index(base);
		const auto adsh_index = adsh_format.sel->get_index(base);
		const auto line_index = line_format.sel->get_index(base);
		if (!stmt_format.validity.AllValid() && !stmt_format.validity.RowIsValidUnsafe(stmt_index)) {
			continue;
		}
		const bool rfile_is_null =
		    !rfile_format.validity.AllValid() && !rfile_format.validity.RowIsValidUnsafe(rfile_index);
		const duckdb::string_t rfile_value = rfile_is_null ? duckdb::string_t() : rfile_values[rfile_index];
		auto &group = state->ResolveGroup(stmt_values[stmt_index], rfile_value, rfile_is_null);
		const bool adsh_valid =
		    adsh_format.validity.AllValid() || adsh_format.validity.RowIsValidUnsafe(adsh_index);
		const bool line_valid =
		    line_format.validity.AllValid() || line_format.validity.RowIsValidUnsafe(line_index);
		state->AggregateRow(group, adsh_values[adsh_index], adsh_valid, line_values[line_index], line_valid, profile);
	}
}

void SecEdgarQ1Kernel::FinalizeGroups() {
	if (state->finalized) {
		return;
	}
	state->ordered_rows.clear();
	state->ordered_rows.reserve(state->active_fixed_slots.size() + state->active_overflow_keys.size());

	for (const auto slot_index : state->active_fixed_slots) {
		const auto &slot = state->fixed_slots[slot_index];
		OutputRow row;
		DecodeFixedSlotLabel(slot_index, row.stmt, row.rfile);
		row.rfile_is_null = slot.rfile_is_null;
		row.cnt = slot.cnt;
		row.num_filings = static_cast<int64_t>(slot.distinct_adsh.size());
		if (slot.line_count > 0) {
			row.has_avg = true;
			row.avg_line_num = static_cast<double>(slot.line_sum) / static_cast<double>(slot.line_count);
		}
		state->ordered_rows.push_back(std::move(row));
	}
	for (const auto &key : state->active_overflow_keys) {
		const auto &slot = state->overflow_groups.at(key);
		OutputRow row;
		row.stmt = slot.stmt;
		row.rfile = slot.rfile;
		row.rfile_is_null = slot.rfile_is_null;
		row.cnt = slot.cnt;
		row.num_filings = static_cast<int64_t>(slot.distinct_adsh.size());
		if (slot.line_count > 0) {
			row.has_avg = true;
			row.avg_line_num = static_cast<double>(slot.line_sum) / static_cast<double>(slot.line_count);
		}
		state->ordered_rows.push_back(std::move(row));
	}

	std::sort(state->ordered_rows.begin(), state->ordered_rows.end(),
	          [](const OutputRow &lhs, const OutputRow &rhs) {
		          if (lhs.cnt != rhs.cnt) {
			          return lhs.cnt > rhs.cnt;
		          }
		          if (lhs.stmt != rhs.stmt) {
			          return lhs.stmt < rhs.stmt;
		          }
		          return lhs.rfile < rhs.rfile;
	          });

	if (profile) {
		profile->active_groups = state->active_fixed_slots.size() + state->active_overflow_keys.size();
		profile->output_groups = state->ordered_rows.size();
	}
	state->finalized = true;
}

void SecEdgarQ1Kernel::Finalize() {
	const auto start = Clock::now();
	FinalizeGroups();
	if (profile) {
		profile->finalize_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
	}
}

bool SecEdgarQ1Kernel::ProduceOutput(duckdb::DataChunk &output) {
	if (!state->finalized || state->output_offset >= state->ordered_rows.size()) {
		return false;
	}
	const auto remaining = state->ordered_rows.size() - state->output_offset;
	const auto count = std::min<duckdb::idx_t>(remaining, kOutputBatchSize);
	for (duckdb::idx_t row = 0; row < count; row++) {
		const auto &result = state->ordered_rows[state->output_offset + row];
		output.SetValue(0, row, duckdb::Value(result.stmt));
		output.SetValue(1, row, result.rfile_is_null ? duckdb::Value() : duckdb::Value(result.rfile));
		output.SetValue(2, row, duckdb::Value::BIGINT(result.cnt));
		output.SetValue(3, row, duckdb::Value::BIGINT(result.num_filings));
		output.SetValue(4, row,
		                result.has_avg ? duckdb::Value::DOUBLE(result.avg_line_num) : duckdb::Value());
	}
	output.SetCardinality(count);
	state->output_offset += count;
	return true;
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1Kernel(const SecEdgarQ1Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ1Kernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1ProfilingKernel(const SecEdgarQ1Parameters &params,
                                                                   SecEdgarQ1ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ1Kernel>(params, &counters);
}

namespace {

constexpr duckdb::idx_t kStmtScanIndex = 0;
constexpr duckdb::idx_t kRfileScanIndex = 1;
constexpr duckdb::idx_t kAdshScanIndex = 2;
constexpr duckdb::idx_t kLineScanIndex = 3;
constexpr duckdb::idx_t kProjectedColumnCount = 4;

} // namespace

SecEdgarQ1FusedKernel::SecEdgarQ1FusedKernel(const SecEdgarQ1Parameters &params, SecEdgarQ1ProfileCounters *profile_p)
    : SecEdgarQ1Kernel(params, profile_p) {
}

void SecEdgarQ1FusedKernel::ConsumeInput(duckdb::idx_t, duckdb::DataChunk &) {
	throw duckdb::InternalException("SEC EDGAR Q1 fused kernel: call RunFusedPreScan instead");
}

void SecEdgarQ1FusedKernel::RunFusedPreScan(duckdb::ClientContext &context, const duckdb::FunctionData &bind_data) {
	if (InputConsumed()) {
		return;
	}
	if (!NativeScanHasFilters(bind_data)) {
		throw duckdb::InvalidInputException(
		    "SEC EDGAR Q1 fused scan requires a pushed stmt IS NOT NULL filter; refusing unsafe TrustFilter");
	}

	const auto scan_start = Clock::now();
	const auto consume_before = profile ? profile->pre_consume_us : 0;

	auto source = CreateNativeChunkSource(context, bind_data);
	while (source->Next()) {
		ConsumePreChunk(source->Chunk());
	}

	MarkInputConsumed();
	if (profile) {
		const auto total_us =
		    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - scan_start).count();
		const auto consume_delta = profile->pre_consume_us - consume_before;
		profile->pre_scan_us += total_us - consume_delta;
	}
}

void RunSecEdgarQ1FusedPreScan(KernelInstance &kernel, duckdb::ClientContext &context,
                               const duckdb::FunctionData &bind_data) {
	auto &fused_kernel = dynamic_cast<SecEdgarQ1FusedKernel &>(kernel);
	fused_kernel.RunFusedPreScan(context, bind_data);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1FusedKernel(const SecEdgarQ1Parameters &params) {
	return duckdb::make_uniq<SecEdgarQ1FusedKernel>(params, nullptr);
}

duckdb::unique_ptr<KernelInstance> CreateSecEdgarQ1FusedProfilingKernel(const SecEdgarQ1Parameters &params,
                                                                       SecEdgarQ1ProfileCounters &counters) {
	return duckdb::make_uniq<SecEdgarQ1FusedKernel>(params, &counters);
}

} // namespace accelerator_codegen
