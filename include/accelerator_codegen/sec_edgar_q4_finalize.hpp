#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/aux_state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ4SicCohortSlot = 0;
inline constexpr aux_state::PartitionKey kSecEdgarQ4PreEqBridgeSlot = 1;
inline constexpr aux_state::PartitionKey kSecEdgarQ4TagTlabelSlot = 2;
inline constexpr aux_state::PartitionKey kSecEdgarQ4GroupAggregatesSlot = 3;
inline constexpr aux_state::PartitionKey kSecEdgarQ4OutputSlot = 4;

using SecEdgarQ4PartitionKey = aux_state::PartitionKey;

struct SecEdgarQ4AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static SecEdgarQ4AdshKey From(const duckdb::string_t &value) {
		SecEdgarQ4AdshKey key;
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

	static SecEdgarQ4AdshKey FromStored(const std::string &value) {
		return From(duckdb::string_t(value));
	}

	bool operator==(const SecEdgarQ4AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct SecEdgarQ4AdshKeyHash {
	size_t operator()(const SecEdgarQ4AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

struct SecEdgarQ4TagVersionKey {
	std::string tag;
	std::string version;

	static SecEdgarQ4TagVersionKey From(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())};
	}

	static SecEdgarQ4TagVersionKey FromStored(const std::string &tag, const std::string &version) {
		return {tag, version};
	}

	bool operator==(const SecEdgarQ4TagVersionKey &other) const {
		return tag == other.tag && version == other.version;
	}
};

struct SecEdgarQ4TagVersionKeyHash {
	size_t operator()(const SecEdgarQ4TagVersionKey &key) const noexcept {
		return std::hash<std::string> {}(key.tag) ^ (std::hash<std::string> {}(key.version) << 1);
	}
};

struct SecEdgarQ4SubInfo {
	int32_t cik = 0;
	int32_t sic = 0;
	uint32_t cik_dense_id = 0;
};

struct SecEdgarQ4SicCohortPartition {
	std::unordered_map<SecEdgarQ4AdshKey, SecEdgarQ4SubInfo, SecEdgarQ4AdshKeyHash> sub_lookup;
	std::unordered_map<int32_t, uint32_t> cik_dense_index;
	uint32_t cik_dense_count = 0;
	uint32_t cik_bit_word_count = 0;
};

struct SecEdgarQ4ResolvedPreTriple {
	uint32_t group_id = 0;
	uint32_t cik_dense_id = 0;
	uint32_t multiplicity = 0;
};

using SecEdgarQ4PreEntriesByTagVersion = std::unordered_map<uint32_t, SecEdgarQ4ResolvedPreTriple>;

struct SecEdgarQ4PreEqBridgePartition {
	std::unordered_map<SecEdgarQ4AdshKey, SecEdgarQ4PreEntriesByTagVersion, SecEdgarQ4AdshKeyHash> pre_by_adsh;
};

struct SecEdgarQ4TagTlabelPartition {
	std::unordered_map<SecEdgarQ4TagVersionKey, uint32_t, SecEdgarQ4TagVersionKeyHash> tag_version_lookup;
	std::vector<uint32_t> tlabel_ids_by_tag_version;
	std::vector<std::string> tlabels;
};

struct SecEdgarQ4GroupKey {
	int32_t sic = 0;
	uint32_t tlabel_id = 0;

	bool operator==(const SecEdgarQ4GroupKey &other) const {
		return sic == other.sic && tlabel_id == other.tlabel_id;
	}
};

struct SecEdgarQ4GroupKeyHash {
	size_t operator()(const SecEdgarQ4GroupKey &key) const noexcept {
		return (static_cast<size_t>(key.sic) << 32) ^ key.tlabel_id;
	}
};

struct SecEdgarQ4GroupAggregate {
	int32_t sic = 0;
	uint32_t tlabel_id = 0;
	double sum = 0.0;
	int64_t count = 0;
	std::vector<uint64_t> cik_bits;
	uint32_t distinct_cik_count = 0;
	bool active = false;
};

struct SecEdgarQ4GroupAggregatesPartition {
	std::unordered_map<SecEdgarQ4GroupKey, uint32_t, SecEdgarQ4GroupKeyHash> group_index;
	std::vector<SecEdgarQ4GroupAggregate> groups;
};

struct SecEdgarQ4OutputRow {
	int32_t sic = 0;
	std::string tlabel;
	std::string stmt = "EQ";
	int64_t num_companies = 0;
	double total_value = 0.0;
	double avg_value = 0.0;
};

struct SecEdgarQ4OutputPartition {
	std::vector<SecEdgarQ4OutputRow> output_rows;
};

std::vector<SecEdgarQ4PartitionKey> SecEdgarQ4FixedPartitionKeys();
void RefreshSecEdgarQ4Output(const SecEdgarQ4GroupAggregatesPartition &aggregates,
                             const SecEdgarQ4TagTlabelPartition &tag_tlabel, SecEdgarQ4OutputPartition &output);
bool ApplySecEdgarQ4UsdNumDelta(const SecEdgarQ4SicCohortPartition &cohort,
                                const SecEdgarQ4PreEqBridgePartition &pre_bridge,
                                const SecEdgarQ4TagTlabelPartition &tag_tlabel, SecEdgarQ4GroupAggregatesPartition &aggregates,
                                SecEdgarQ4OutputPartition &output, const SecEdgarQ4AdshKey &adsh_key,
                                const SecEdgarQ4TagVersionKey &tag_version_key, double value, int8_t sign);

} // namespace accelerator_codegen
