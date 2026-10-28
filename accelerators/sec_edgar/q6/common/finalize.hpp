#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/state/partition_key.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace accelerator_codegen {

inline constexpr aux_state::PartitionKey kSecEdgarQ6Fy2023CohortSlot = 0;
inline constexpr aux_state::PartitionKey kSecEdgarQ6PreIsBridgeSlot = 1;
inline constexpr aux_state::PartitionKey kSecEdgarQ6TagVersionSlot = 2;
inline constexpr aux_state::PartitionKey kSecEdgarQ6GroupAggregatesSlot = 3;
inline constexpr aux_state::PartitionKey kSecEdgarQ6OutputSlot = 4;

using SecEdgarQ6PartitionKey = aux_state::PartitionKey;

struct SecEdgarQ6AdshKey {
	uint32_t size = 0;
	uint64_t w0 = 0;
	uint64_t w1 = 0;
	uint64_t w2 = 0;

	static SecEdgarQ6AdshKey From(const duckdb::string_t &value) {
		SecEdgarQ6AdshKey key;
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

	static SecEdgarQ6AdshKey FromStored(const std::string &value) {
		return From(duckdb::string_t(value));
	}

	bool operator==(const SecEdgarQ6AdshKey &other) const {
		return size == other.size && w0 == other.w0 && w1 == other.w1 && w2 == other.w2;
	}
};

struct SecEdgarQ6AdshKeyHash {
	size_t operator()(const SecEdgarQ6AdshKey &key) const noexcept {
		return key.w0 ^ (key.w1 << 1) ^ (key.w2 << 2) ^ key.size;
	}
};

struct SecEdgarQ6TagVersionKey {
	std::string tag;
	std::string version;

	static SecEdgarQ6TagVersionKey From(const duckdb::string_t &tag, const duckdb::string_t &version) {
		return {std::string(tag.GetData(), tag.GetSize()), std::string(version.GetData(), version.GetSize())};
	}

	static SecEdgarQ6TagVersionKey FromStored(const std::string &tag, const std::string &version) {
		return {tag, version};
	}

	bool operator==(const SecEdgarQ6TagVersionKey &other) const {
		return tag == other.tag && version == other.version;
	}
};

struct SecEdgarQ6TagVersionKeyHash {
	size_t operator()(const SecEdgarQ6TagVersionKey &key) const noexcept {
		return std::hash<std::string> {}(key.tag) ^ (std::hash<std::string> {}(key.version) << 1);
	}
};

struct SecEdgarQ6Fy2023CohortPartition {
	std::unordered_map<SecEdgarQ6AdshKey, uint32_t, SecEdgarQ6AdshKeyHash> adsh_to_name_id;
	std::vector<std::string> names;
};

struct SecEdgarQ6ResolvedPreEntry {
	uint32_t tag_id = 0;
	uint32_t plabel_id = 0;
	uint32_t multiplicity = 0;
	uint32_t group_id = 0;
};

struct SecEdgarQ6ResolvedPre {
	bool single = false;
	uint32_t single_group_id = 0;
	uint32_t single_multiplicity = 0;
	std::vector<SecEdgarQ6ResolvedPreEntry> entries;
};

using SecEdgarQ6PreEntriesByTagVersion = std::unordered_map<uint32_t, SecEdgarQ6ResolvedPre>;

struct SecEdgarQ6PreIsBridgePartition {
	std::unordered_map<SecEdgarQ6AdshKey, SecEdgarQ6PreEntriesByTagVersion, SecEdgarQ6AdshKeyHash> pre_by_adsh;
};

struct SecEdgarQ6TagVersionPartition {
	std::unordered_map<SecEdgarQ6TagVersionKey, uint32_t, SecEdgarQ6TagVersionKeyHash> tag_version_lookup;
	std::vector<std::pair<std::string, std::string>> tag_versions;
};

struct SecEdgarQ6GroupMeta {
	uint32_t name_id = 0;
	uint32_t tag_id = 0;
	uint32_t plabel_id = 0;
};

struct SecEdgarQ6GroupAggregate {
	double sum = 0.0;
	int64_t count = 0;
	bool active = false;
};

struct SecEdgarQ6GroupAggregatesPartition {
	std::vector<SecEdgarQ6GroupMeta> group_meta;
	std::vector<SecEdgarQ6GroupAggregate> groups;
	std::vector<std::string> tags;
	std::vector<std::string> plabels;
};

struct SecEdgarQ6OutputRow {
	std::string name;
	std::string tag;
	std::string plabel;
	std::string stmt;
	double total_value = 0.0;
	int64_t cnt = 0;
};

struct SecEdgarQ6OutputPartition {
	std::vector<SecEdgarQ6OutputRow> output_rows;
};

} // namespace accelerator_codegen
