#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "substrate/aux/state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ3Fy2022CohortSlot = 0;
inline constexpr aux_state::PartitionKey kSecEdgarQ3CikTotalsSlot = 1;
inline constexpr aux_state::PartitionKey kSecEdgarQ3OutputSlot = 2;

using SecEdgarQ3PartitionKey = aux_state::PartitionKey;

struct SecEdgarQ3AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;
	uint64_t w3 = 0;

	static SecEdgarQ3AdshKey From(const duckdb::string_t &value) {
		SecEdgarQ3AdshKey key;
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

	static SecEdgarQ3AdshKey FromStored(const std::string &value) {
		return From(duckdb::string_t(value));
	}

	bool operator==(const SecEdgarQ3AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2 && w3 == other.w3;
	}
};

struct SecEdgarQ3AdshKeyHash {
	size_t operator()(const SecEdgarQ3AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ (key.w3 << 3) ^ key.size;
	}
};

struct SecEdgarQ3FilingInfo {
	std::string name;
	int32_t cik = 0;
	uint32_t group_id = 0;
};

struct SecEdgarQ3Fy2022CohortPartition {
	std::unordered_map<SecEdgarQ3AdshKey, SecEdgarQ3FilingInfo, SecEdgarQ3AdshKeyHash> adsh_to_filing;
};

struct SecEdgarQ3CikTotalsPartition {
	std::unordered_map<int32_t, double> cik_totals;
	std::vector<std::string> group_names;
	std::vector<int32_t> group_ciks;
	std::vector<double> group_sums;
};

struct SecEdgarQ3OutputRow {
	std::string name;
	int32_t cik = 0;
	double total_value = 0.0;
};

struct SecEdgarQ3OutputPartition {
	std::vector<SecEdgarQ3OutputRow> output_rows;
};

struct SecEdgarQ3NameCikKey {
	std::string name;
	int32_t cik = 0;

	bool operator==(const SecEdgarQ3NameCikKey &other) const {
		return cik == other.cik && name == other.name;
	}
};

struct SecEdgarQ3NameCikKeyHash {
	size_t operator()(const SecEdgarQ3NameCikKey &key) const noexcept {
		return std::hash<std::string> {}(key.name) ^ (static_cast<size_t>(key.cik) << 1);
	}
};

std::vector<SecEdgarQ3PartitionKey> SecEdgarQ3FixedPartitionKeys();
std::vector<SecEdgarQ3OutputRow> FinalizeSecEdgarQ3Groups(const std::unordered_map<int32_t, double> &cik_totals,
                                                          const std::vector<std::string> &group_names,
                                                          const std::vector<int32_t> &group_ciks,
                                                          const std::vector<double> &group_sums);
void RefreshSecEdgarQ3Output(SecEdgarQ3CikTotalsPartition &aggregates, SecEdgarQ3OutputPartition &output);
bool ApplySecEdgarQ3UsdNumDelta(SecEdgarQ3Fy2022CohortPartition &cohort, SecEdgarQ3CikTotalsPartition &aggregates,
                                SecEdgarQ3OutputPartition &output, const SecEdgarQ3AdshKey &adsh_key, double value,
                                int8_t sign);

} // namespace accelerator_codegen
