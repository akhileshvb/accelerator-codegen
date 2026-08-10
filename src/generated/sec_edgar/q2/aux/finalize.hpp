#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ2MaxPureMapSlot = 0;
inline constexpr aux_state::PartitionKey kSecEdgarQ2Fy2022CohortSlot = 1;
inline constexpr aux_state::PartitionKey kSecEdgarQ2MaxCandidatesSlot = 2;

using SecEdgarQ2PartitionKey = aux_state::PartitionKey;

struct SecEdgarQ2AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;
	uint64_t w3 = 0;

	static SecEdgarQ2AdshKey From(const duckdb::string_t &value) {
		SecEdgarQ2AdshKey key;
		key.size = static_cast<uint32_t>(value.GetSize());
		const auto *data = value.GetData();
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

	static SecEdgarQ2AdshKey FromStored(const std::string &value) {
		return From(duckdb::string_t(value));
	}

	bool operator==(const SecEdgarQ2AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2 && w3 == other.w3;
	}
};

struct SecEdgarQ2AdshKeyHash {
	size_t operator()(const SecEdgarQ2AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ (key.w3 << 3) ^ key.size;
	}
};

struct SecEdgarQ2GroupKey {
	SecEdgarQ2AdshKey adsh;
	SecEdgarQ2AdshKey tag;

	static SecEdgarQ2GroupKey From(const duckdb::string_t &adsh, const duckdb::string_t &tag) {
		return {SecEdgarQ2AdshKey::From(adsh), SecEdgarQ2AdshKey::From(tag)};
	}

	static SecEdgarQ2GroupKey FromStored(const std::string &adsh, const std::string &tag) {
		return {SecEdgarQ2AdshKey::FromStored(adsh), SecEdgarQ2AdshKey::FromStored(tag)};
	}

	bool operator==(const SecEdgarQ2GroupKey &other) const {
		return adsh == other.adsh && tag == other.tag;
	}
};

struct SecEdgarQ2GroupKeyHash {
	size_t operator()(const SecEdgarQ2GroupKey &key) const noexcept {
		return SecEdgarQ2AdshKeyHash {}(key.adsh) ^ (SecEdgarQ2AdshKeyHash {}(key.tag) << 1);
	}
};

struct SecEdgarQ2CandidateRow {
	std::string name;
	std::string tag;
	double value = 0.0;
};

struct SecEdgarQ2OutputRow {
	std::string name;
	std::string tag;
	double value = 0.0;
};

struct SecEdgarQ2MaxPureMapPartition {
	std::unordered_map<SecEdgarQ2GroupKey, double, SecEdgarQ2GroupKeyHash> max_by_adsh_tag;
};

struct SecEdgarQ2Fy2022CohortPartition {
	std::unordered_map<SecEdgarQ2AdshKey, std::string, SecEdgarQ2AdshKeyHash> adsh_to_name;
};

struct SecEdgarQ2CandidatePartition {
	std::vector<SecEdgarQ2CandidateRow> rows_at_max;
	std::vector<SecEdgarQ2OutputRow> output_rows;
};

std::vector<SecEdgarQ2PartitionKey> SecEdgarQ2FixedPartitionKeys();
std::vector<SecEdgarQ2OutputRow> FinalizeSecEdgarQ2Candidates(std::vector<SecEdgarQ2CandidateRow> candidates);
void RefreshSecEdgarQ2CandidateOutput(SecEdgarQ2CandidatePartition &candidates);
bool ApplySecEdgarQ2PureNumInsert(SecEdgarQ2MaxPureMapPartition &max_map, SecEdgarQ2Fy2022CohortPartition &cohort,
                                  SecEdgarQ2CandidatePartition &candidates, const SecEdgarQ2AdshKey &adsh_key,
                                  const std::string &tag, double value);

} // namespace accelerator_codegen
